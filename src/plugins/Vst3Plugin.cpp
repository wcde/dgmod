// VST 3 effect hosting: component + edit controller (one object or two connected ones), parameter changes from the
// editor queued to the processor, parameter output echoed back to the controller, state = component + controller.

#include "plugins/Plugin.h"
#include "plugins/VstAbi.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <variant>

namespace dgmod::plugins {

namespace {

using namespace vst3;

constexpr uint32_t kStateMagic = 0x33564744;  // "DGV3"

std::wstring FromUtf8(const char* s, size_t max) { return Widen(std::string_view(s, strnlen(s, max))); }

// Parameter changes crossing threads: a bounded ring, dropped when full (a later change of the same parameter
// supersedes it anyway). Push is serialized by a mutex only on the host side (the editor may call from any thread);
// the render thread never waits on it.
class ParamRing {
public:
    struct Item {
        ParamID id;
        ParamValue value;
    };
    void Push(ParamID id, ParamValue v) {
        const uint32_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) >= kSize) return;
        items_[h % kSize] = {id, v};
        head_.store(h + 1, std::memory_order_release);
    }
    void PushLocked(ParamID id, ParamValue v) {
        std::lock_guard lock(mutex_);
        Push(id, v);
    }
    template <class F>
    void Drain(F&& f) {
        uint32_t t = tail_.load(std::memory_order_relaxed);
        const uint32_t h = head_.load(std::memory_order_acquire);
        for (; t != h; ++t) f(items_[t % kSize]);
        tail_.store(t, std::memory_order_release);
    }

private:
    static constexpr uint32_t kSize = 1024;
    std::array<Item, kSize> items_{};
    std::atomic<uint32_t> head_{0}, tail_{0};
    std::mutex mutex_;
};

// ---- Host-side COM objects ----------------------------------------------------------------------------------------

// Objects owned by the host (members of the plug-in instance): reference counting is a formality.
#define DGMOD_OWNED_REFCOUNT                                    \
    uint32_t DGMOD_VST3_API addRef() override { return 1; }  \
    uint32_t DGMOD_VST3_API release() override { return 1; }

class AttributeList final : public IAttributeList {
public:
    tresult DGMOD_VST3_API queryInterface(const char* iid, void** obj) override {
        if (kIidFUnknown.Is(iid) || kIidIAttributeList.Is(iid)) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32_t DGMOD_VST3_API addRef() override { return ++refs_; }
    uint32_t DGMOD_VST3_API release() override {
        const uint32_t r = --refs_;
        if (!r) delete this;
        return r;
    }
    tresult DGMOD_VST3_API setInt(const char* id, int64_t v) override { return Set(id, v); }
    tresult DGMOD_VST3_API getInt(const char* id, int64_t& v) override { return Get(id, v); }
    tresult DGMOD_VST3_API setFloat(const char* id, double v) override { return Set(id, v); }
    tresult DGMOD_VST3_API getFloat(const char* id, double& v) override { return Get(id, v); }
    tresult DGMOD_VST3_API setString(const char* id, const char16* s) override {
        return s ? Set(id, std::u16string(s)) : kInvalidArgument;
    }
    tresult DGMOD_VST3_API getString(const char* id, char16* s, uint32_t sizeInBytes) override {
        std::u16string v;
        if (Get(id, v) != kResultOk || !s || sizeInBytes < 2) return kResultFalse;
        const size_t n = std::min<size_t>(v.size(), sizeInBytes / 2 - 1);
        std::memcpy(s, v.data(), n * 2);
        s[n] = 0;
        return kResultOk;
    }
    tresult DGMOD_VST3_API setBinary(const char* id, const void* data, uint32_t size) override {
        const auto* p = static_cast<const uint8_t*>(data);
        return Set(id, std::vector<uint8_t>(p, p + (p ? size : 0)));
    }
    tresult DGMOD_VST3_API getBinary(const char* id, const void*& data, uint32_t& size) override {
        if (!id) return kInvalidArgument;
        const auto it = values_.find(id);
        if (it == values_.end() || !std::holds_alternative<std::vector<uint8_t>>(it->second)) return kResultFalse;
        const auto& v = std::get<std::vector<uint8_t>>(it->second);
        data = v.data();
        size = static_cast<uint32_t>(v.size());
        return kResultOk;
    }

private:
    using Value = std::variant<int64_t, double, std::u16string, std::vector<uint8_t>>;
    template <class T>
    tresult Set(const char* id, T v) {
        if (!id) return kInvalidArgument;
        values_[id] = std::move(v);
        return kResultOk;
    }
    template <class T>
    tresult Get(const char* id, T& out) {
        if (!id) return kInvalidArgument;
        const auto it = values_.find(id);
        if (it == values_.end() || !std::holds_alternative<T>(it->second)) return kResultFalse;
        out = std::get<T>(it->second);
        return kResultOk;
    }
    std::atomic<uint32_t> refs_{1};
    std::map<std::string, Value> values_;
};

class Message final : public IMessage {
public:
    tresult DGMOD_VST3_API queryInterface(const char* iid, void** obj) override {
        if (kIidFUnknown.Is(iid) || kIidIMessage.Is(iid)) {
            addRef();
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32_t DGMOD_VST3_API addRef() override { return ++refs_; }
    uint32_t DGMOD_VST3_API release() override {
        const uint32_t r = --refs_;
        if (!r) delete this;
        return r;
    }
    FIDString DGMOD_VST3_API getMessageID() override { return id_.c_str(); }
    void DGMOD_VST3_API setMessageID(FIDString id) override { id_ = id ? id : ""; }
    IAttributeList* DGMOD_VST3_API getAttributes() override { return attributes_; }
    ~Message() { attributes_->release(); }

private:
    std::atomic<uint32_t> refs_{1};
    std::string id_;
    AttributeList* attributes_ = new AttributeList;
};

class HostApplication final : public IHostApplication {
public:
    tresult DGMOD_VST3_API queryInterface(const char* iid, void** obj) override {
        if (kIidFUnknown.Is(iid) || kIidIHostApplication.Is(iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    DGMOD_OWNED_REFCOUNT
    tresult DGMOD_VST3_API getName(String128 name) override {
        const char16_t kName[] = u"dgmod";
        std::memcpy(name, kName, sizeof(kName));
        return kResultOk;
    }
    tresult DGMOD_VST3_API createInstance(char cid[16], char iid[16], void** obj) override {
        if (kIidIMessage.Is(cid) && kIidIMessage.Is(iid)) {
            *obj = static_cast<IMessage*>(new Message);
            return kResultOk;
        }
        if (kIidIAttributeList.Is(cid) && kIidIAttributeList.Is(iid)) {
            *obj = static_cast<IAttributeList*>(new AttributeList);
            return kResultOk;
        }
        *obj = nullptr;
        return kResultFalse;
    }
};

// Growable in-memory stream for component and controller state.
class MemoryStream final : public IBStream {
public:
    MemoryStream() = default;
    explicit MemoryStream(std::span<const uint8_t> data) : data_(data.begin(), data.end()) {}
    tresult DGMOD_VST3_API queryInterface(const char* iid, void** obj) override {
        if (kIidFUnknown.Is(iid) || kIidIBStream.Is(iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    DGMOD_OWNED_REFCOUNT
    tresult DGMOD_VST3_API read(void* buffer, int32_t n, int32_t* got) override {
        if (n < 0 || (!buffer && n)) return kInvalidArgument;
        const size_t k = std::min<size_t>(size_t(n), data_.size() - std::min(pos_, data_.size()));
        if (k) std::memcpy(buffer, data_.data() + pos_, k);
        pos_ += k;
        if (got) *got = static_cast<int32_t>(k);
        return kResultOk;
    }
    tresult DGMOD_VST3_API write(void* buffer, int32_t n, int32_t* written) override {
        if (n < 0 || (!buffer && n)) return kInvalidArgument;
        if (pos_ + size_t(n) > data_.size()) data_.resize(pos_ + size_t(n));
        if (n) std::memcpy(data_.data() + pos_, buffer, size_t(n));
        pos_ += size_t(n);
        if (written) *written = n;
        return kResultOk;
    }
    tresult DGMOD_VST3_API seek(int64_t pos, int32_t mode, int64_t* result) override {
        int64_t base = mode == kIBSeekSet ? 0 : mode == kIBSeekCur ? int64_t(pos_) : int64_t(data_.size());
        const int64_t p = base + pos;
        if (p < 0) return kInvalidArgument;
        pos_ = size_t(p);
        if (result) *result = p;
        return kResultOk;
    }
    tresult DGMOD_VST3_API tell(int64_t* pos) override {
        if (!pos) return kInvalidArgument;
        *pos = int64_t(pos_);
        return kResultOk;
    }
    void Rewind() { pos_ = 0; }
    [[nodiscard]] const std::vector<uint8_t>& Data() const { return data_; }

private:
    std::vector<uint8_t> data_;
    size_t pos_ = 0;
};

class ParamQueue final : public IParamValueQueue {
public:
    tresult DGMOD_VST3_API queryInterface(const char* iid, void** obj) override {
        if (kIidFUnknown.Is(iid) || kIidIParamValueQueue.Is(iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    DGMOD_OWNED_REFCOUNT
    ParamID DGMOD_VST3_API getParameterId() override { return id; }
    int32_t DGMOD_VST3_API getPointCount() override { return count; }
    tresult DGMOD_VST3_API getPoint(int32_t index, int32_t& offset, ParamValue& value) override {
        if (index < 0 || index >= count) return kInvalidArgument;
        offset = points[size_t(index)].offset;
        value = points[size_t(index)].value;
        return kResultOk;
    }
    tresult DGMOD_VST3_API addPoint(int32_t offset, ParamValue value, int32_t& index) override {
        // Points stay ordered by offset; a point at an offset already there replaces it.
        int32_t at = count;
        while (at > 0 && points[size_t(at) - 1].offset > offset) --at;
        if (at > 0 && points[size_t(at) - 1].offset == offset) {
            points[size_t(at) - 1].value = value;
            index = at - 1;
            return kResultOk;
        }
        if (count == kMaxPoints) return kResultFalse;
        for (int32_t i = count; i > at; --i) points[size_t(i)] = points[size_t(i) - 1];
        points[size_t(at)] = {offset, value};
        ++count;
        index = at;
        return kResultOk;
    }
    static constexpr int32_t kMaxPoints = 32;
    struct Point {
        int32_t offset;
        ParamValue value;
    };
    ParamID id = 0;
    int32_t count = 0;
    std::array<Point, kMaxPoints> points{};
};

class ParamChanges final : public IParameterChanges {
public:
    tresult DGMOD_VST3_API queryInterface(const char* iid, void** obj) override {
        if (kIidFUnknown.Is(iid) || kIidIParameterChanges.Is(iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    DGMOD_OWNED_REFCOUNT
    int32_t DGMOD_VST3_API getParameterCount() override { return used_; }
    IParamValueQueue* DGMOD_VST3_API getParameterData(int32_t index) override {
        return index >= 0 && index < used_ ? &queues_[size_t(index)] : nullptr;
    }
    IParamValueQueue* DGMOD_VST3_API addParameterData(const ParamID& id, int32_t& index) override {
        for (int32_t i = 0; i < used_; ++i)
            if (queues_[size_t(i)].id == id) {
                index = i;
                return &queues_[size_t(i)];
            }
        if (used_ == kMaxQueues) return nullptr;
        ParamQueue& q = queues_[size_t(used_)];
        q.id = id;
        q.count = 0;
        index = used_++;
        return &q;
    }
    void Clear() { used_ = 0; }
    [[nodiscard]] int32_t Used() const { return used_; }
    [[nodiscard]] const ParamQueue& Queue(int32_t i) const { return queues_[size_t(i)]; }

private:
    static constexpr int32_t kMaxQueues = 128;
    std::array<ParamQueue, kMaxQueues> queues_{};
    int32_t used_ = 0;
};

class EmptyEvents final : public IEventList {
public:
    tresult DGMOD_VST3_API queryInterface(const char* iid, void** obj) override {
        if (kIidFUnknown.Is(iid) || kIidIEventList.Is(iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    DGMOD_OWNED_REFCOUNT
    int32_t DGMOD_VST3_API getEventCount() override { return 0; }
    tresult DGMOD_VST3_API getEvent(int32_t, void*) override { return kResultFalse; }
    tresult DGMOD_VST3_API addEvent(void*) override { return kResultOk; }
};

uint32_t Channels(SpeakerArrangement a) { return static_cast<uint32_t>(std::popcount(a)); }

// ---- The instance ---------------------------------------------------------------------------------------------------

class Vst3Plugin final : public PluginInstance {
public:
    explicit Vst3Plugin(PluginCallbacks* cb) : handler_(this), frame_(this) {
        callbacks_ = cb;
        format_ = PluginFormat::Vst3;
    }

    ~Vst3Plugin() override {
        if (Crashed()) return;  // leave a crashed module alone
        CloseEditor();
        Deactivate();
        Call(L"unloading", [&] {
            if (componentCp_ && controllerCp_) {
                componentCp_->disconnect(controllerCp_);
                controllerCp_->disconnect(componentCp_);
            }
            if (componentCp_) componentCp_->release();
            if (controllerCp_) controllerCp_->release();
            if (controller_) {
                controller_->setComponentHandler(nullptr);
                if (separateController_) controller_->terminate();
                controller_->release();
            }
            if (processor_) processor_->release();
            if (component_) {
                component_->terminate();
                component_->release();
            }
            if (factory_) factory_->release();
        });
        if (Crashed()) return;
        if (module_) {
            if (auto exitDll = reinterpret_cast<InitModuleProc>(::GetProcAddress(module_, "ExitDll"))) Call(L"ExitDll", [&] { exitDll(); });
            if (!Crashed()) ::FreeLibrary(module_);
        }
    }

    Result<void> Init(const std::wstring& path, const std::wstring& classId) {
        std::wstring error;
        module_ = LoadPluginModule(PluginBinaryPath(path), error);
        if (!module_) return Fail(E_FAIL, error);
        auto getFactory = reinterpret_cast<GetFactoryProc>(::GetProcAddress(module_, "GetPluginFactory"));
        if (!getFactory) return Fail(E_FAIL, L"Not a VST 3 plug-in (no GetPluginFactory)");
        bool ok = true;
        if (auto initDll = reinterpret_cast<InitModuleProc>(::GetProcAddress(module_, "InitDll")))
            if (!Call(L"InitDll", [&] { ok = initDll(); }) || !ok) return Fail(E_FAIL, L"The plug-in failed to initialize");
        if (!Call(L"GetPluginFactory", [&] { factory_ = getFactory(); }) || !factory_)
            return Fail(E_FAIL, L"The plug-in has no factory");

        // The class: the configured one, else the first audio effect.
        char cid[16]{};
        const bool wanted = ParseClassId(classId, cid);
        bool found = false;
        PFactoryInfo fi{};
        IPluginFactory2* f2 = nullptr;
        Call(L"factory", [&] {
            factory_->getFactoryInfo(&fi);
            factory_->queryInterface(kIidIPluginFactory2.bytes, reinterpret_cast<void**>(&f2));
            const int32_t n = factory_->countClasses();
            for (int32_t i = 0; i < n && !found; ++i) {
                PClassInfo2 info2{};
                PClassInfo info{};
                if (f2 && f2->getClassInfo2(i, &info2) == kResultOk) {
                    std::memcpy(info.cid, info2.cid, 16);
                    std::memcpy(info.category, info2.category, sizeof(info.category));
                    std::memcpy(info.name, info2.name, sizeof(info.name));
                } else if (factory_->getClassInfo(i, &info) != kResultOk) {
                    continue;
                }
                if (std::strncmp(info.category, kAudioEffectClass, sizeof(info.category)) != 0) continue;
                if (wanted && std::memcmp(info.cid, cid, 16) != 0) continue;
                std::memcpy(cid, info.cid, 16);
                name_ = FromUtf8(info.name, sizeof(info.name));
                vendor_ = f2 && info2.vendor[0] ? FromUtf8(info2.vendor, sizeof(info2.vendor)) : FromUtf8(fi.vendor, sizeof(fi.vendor));
                found = true;
            }
            if (f2) f2->release();
        });
        if (Crashed()) return Fail(E_FAIL, L"The plug-in crashed while listing its classes: " + ExceptionName(CrashCode()));
        if (!found) return Fail(E_FAIL, wanted ? L"The plug-in no longer contains this effect" : L"The plug-in contains no audio effect");
        if (name_.empty()) name_ = std::filesystem::path(path).stem().wstring();

        tresult r = kResultFalse;
        Call(L"createInstance", [&] { r = factory_->createInstance(cid, kIidIComponent.bytes, reinterpret_cast<void**>(&component_)); });
        if (r != kResultOk || !component_) return Fail(E_FAIL, L"The plug-in could not create its effect");
        Call(L"initialize", [&] { r = component_->initialize(&host_); });
        if (Crashed() || r != kResultOk) {
            if (!Crashed()) {
                component_->release();
                component_ = nullptr;
            }
            return Fail(E_FAIL, L"The effect failed to initialize");
        }
        Call(L"initialize", [&] {
            component_->queryInterface(kIidIAudioProcessor.bytes, reinterpret_cast<void**>(&processor_));
            if (component_->queryInterface(kIidIEditController.bytes, reinterpret_cast<void**>(&controller_)) != kResultOk ||
                !controller_) {
                controller_ = nullptr;
                char ccid[16]{};
                if (component_->getControllerClassId(ccid) == kResultOk &&
                    factory_->createInstance(ccid, kIidIEditController.bytes, reinterpret_cast<void**>(&controller_)) == kResultOk &&
                    controller_) {
                    separateController_ = true;
                    if (controller_->initialize(&host_) != kResultOk) {
                        controller_->release();
                        controller_ = nullptr;
                        separateController_ = false;
                    }
                }
            }
            if (processor_ && processor_->canProcessSampleSize(kSample32) != kResultTrue) float32_ = false;
            if (separateController_) {
                component_->queryInterface(kIidIConnectionPoint.bytes, reinterpret_cast<void**>(&componentCp_));
                controller_->queryInterface(kIidIConnectionPoint.bytes, reinterpret_cast<void**>(&controllerCp_));
                if (componentCp_ && controllerCp_) {
                    componentCp_->connect(controllerCp_);
                    controllerCp_->connect(componentCp_);
                }
            }
            if (controller_) {
                controller_->setComponentHandler(&handler_);
                MemoryStream s;
                if (component_->getState(&s) == kResultOk) {
                    s.Rewind();
                    controller_->setComponentState(&s);
                }
            }
        });
        if (Crashed()) return Fail(E_FAIL, L"The plug-in crashed while initializing: " + ExceptionName(CrashCode()));
        if (!processor_) return Fail(E_FAIL, L"The effect has no audio processor");
        if (!float32_) return Fail(E_FAIL, L"The effect only processes 64-bit samples");
        return {};
    }

    Result<void> Activate(const ProcessFormat& f) override {
        if (Crashed()) return Fail(E_FAIL, L"The plug-in crashed");
        Deactivate();
        processFormat_ = f;
        tresult setup = kResultFalse;
        int32_t nIn = 0, nOut = 0;
        std::vector<SpeakerArrangement> inArr, outArr;
        Call(L"activating", [&] {
            nIn = component_->getBusCount(kAudio, kInput);
            nOut = component_->getBusCount(kAudio, kOutput);
            inArr.assign(size_t(std::max(nIn, 0)), kSpeakerArrStereo);
            outArr.assign(size_t(std::max(nOut, 0)), kSpeakerArrStereo);
            auto current = [&] {
                for (int32_t i = 0; i < nIn; ++i) processor_->getBusArrangement(kInput, i, inArr[size_t(i)]);
                for (int32_t i = 0; i < nOut; ++i) processor_->getBusArrangement(kOutput, i, outArr[size_t(i)]);
            };
            current();
            const SpeakerArrangement want = f.channels == 1 ? kSpeakerArrMono : kSpeakerArrStereo;
            if (nIn > 0) inArr[0] = want;
            if (nOut > 0) outArr[0] = want;
            if (processor_->setBusArrangements(inArr.data(), nIn, outArr.data(), nOut) != kResultTrue) current();
            for (int32_t i = 0; i < nIn; ++i) component_->activateBus(kAudio, kInput, i, i == 0);
            for (int32_t i = 0; i < nOut; ++i) component_->activateBus(kAudio, kOutput, i, i == 0);
            ProcessSetup ps{kRealtime, kSample32, static_cast<int32_t>(f.maxFrames), f.rate};
            setup = processor_->setupProcessing(ps);
            if (setup == kResultOk) {
                component_->setActive(1);
                latency_ = processor_->getLatencySamples();
                processor_->setProcessing(1);
            }
        });
        if (Crashed()) return Fail(E_FAIL, L"The plug-in crashed while starting: " + ExceptionName(CrashCode()));
        if (setup != kResultOk) return Fail(E_FAIL, L"The effect refused the stream format");
        if (nOut <= 0) {
            Deactivate();
            return Fail(E_FAIL, L"The effect has no audio output");
        }
        // Buffers for every bus (aux inputs get silence, extra outputs are discarded).
        size_t total = 0;
        for (const auto a : inArr) total += Channels(a);
        for (const auto a : outArr) total += Channels(a);
        storage_.assign(total * f.maxFrames, 0.0f);
        ptrs_.assign(total, nullptr);
        inBuses_.assign(inArr.size(), {});
        outBuses_.assign(outArr.size(), {});
        size_t k = 0;
        auto wire = [&](std::vector<AudioBusBuffers>& buses, const std::vector<SpeakerArrangement>& arr) {
            for (size_t b = 0; b < arr.size(); ++b) {
                buses[b].numChannels = static_cast<int32_t>(Channels(arr[b]));
                buses[b].silenceFlags = 0;
                buses[b].channelBuffers32 = ptrs_.data() + k;
                for (uint32_t c = 0; c < Channels(arr[b]); ++c, ++k) ptrs_[k] = storage_.data() + k * f.maxFrames;
            }
        };
        wire(inBuses_, inArr);
        wire(outBuses_, outArr);
        inputs_ = inArr.empty() ? 0 : Channels(inArr[0]);
        outputs_ = Channels(outArr[0]);
        if (!outputs_) {
            Deactivate();
            return Fail(E_FAIL, L"The effect's main output has no channels");
        }
        std::memset(&context_, 0, sizeof(context_));
        context_.sampleRate = f.rate;
        context_.tempo = 120.0;
        context_.timeSigNumerator = context_.timeSigDenominator = 4;
        context_.state = ProcessContext::kPlaying | ProcessContext::kTempoValid | ProcessContext::kTimeSigValid |
                         ProcessContext::kContTimeValid | ProcessContext::kProjectTimeMusicValid;
        std::memset(&data_, 0, sizeof(data_));
        data_.processMode = kRealtime;
        data_.symbolicSampleSize = kSample32;
        data_.numInputs = static_cast<int32_t>(inBuses_.size());
        data_.numOutputs = static_cast<int32_t>(outBuses_.size());
        data_.inputs = inBuses_.empty() ? nullptr : inBuses_.data();
        data_.outputs = outBuses_.data();
        data_.inputParameterChanges = &inChanges_;
        data_.outputParameterChanges = &outChanges_;
        data_.inputEvents = &events_;
        data_.outputEvents = nullptr;
        data_.processContext = &context_;
        active_ = true;
        return {};
    }

    void Deactivate() override {
        if (!active_) return;
        active_ = false;
        Call(L"deactivating", [&] {
            processor_->setProcessing(0);
            component_->setActive(0);
        });
    }

    std::vector<uint8_t> SaveState() override {
        std::vector<uint8_t> out;
        if (Crashed()) return out;
        MemoryStream comp, ctrl;
        Call(L"saving the state", [&] {
            component_->getState(&comp);
            if (controller_) controller_->getState(&ctrl);
        });
        if (Crashed()) return out;
        auto put = [&](uint32_t v) { out.insert(out.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); };
        put(kStateMagic);
        put(static_cast<uint32_t>(comp.Data().size()));
        out.insert(out.end(), comp.Data().begin(), comp.Data().end());
        put(static_cast<uint32_t>(ctrl.Data().size()));
        out.insert(out.end(), ctrl.Data().begin(), ctrl.Data().end());
        return out;
    }

    Result<void> LoadState(std::span<const uint8_t> data) override {
        if (Crashed()) return Fail(E_FAIL, L"The plug-in crashed");
        auto get = [&](size_t at) {
            uint32_t v = 0;
            if (at + 4 <= data.size()) std::memcpy(&v, data.data() + at, 4);
            return v;
        };
        if (data.size() < 12 || get(0) != kStateMagic) return Fail(E_INVALIDARG, L"Unknown state format");
        const size_t n1 = get(4);
        if (8 + n1 + 4 > data.size()) return Fail(E_INVALIDARG, L"Truncated state");
        const size_t n2 = get(8 + n1);
        if (12 + n1 + n2 > data.size()) return Fail(E_INVALIDARG, L"Truncated state");
        MemoryStream comp(data.subspan(8, n1)), ctrl(data.subspan(12 + n1, n2));
        Call(L"loading the state", [&] {
            if (n1) component_->setState(&comp);
            if (controller_) {
                if (n1) {
                    comp.Rewind();
                    controller_->setComponentState(&comp);
                }
                if (n2) controller_->setState(&ctrl);
            }
        });
        if (Crashed()) return Fail(E_FAIL, L"The plug-in crashed while loading its state: " + ExceptionName(CrashCode()));
        return {};
    }

    [[nodiscard]] bool HasEditor() const override { return controller_ != nullptr && !noEditor_; }

    Result<SIZE> OpenEditor(HWND parent, float scale) override {
        if (!HasEditor()) return Fail(E_NOTIMPL, L"The plug-in has no editor");
        if (Crashed()) return Fail(E_FAIL, L"The plug-in crashed");
        CloseEditor();
        tresult attached = kResultFalse;
        ViewRect r{0, 0, 640, 400};
        Call(L"opening the editor", [&] {
            view_ = controller_->createView(kEditorView);
            if (!view_) return;
            if (view_->isPlatformTypeSupported(kPlatformTypeHwnd) != kResultTrue) return;
            view_->setFrame(&frame_);
            if (view_->queryInterface(kIidIPlugViewContentScaleSupport.bytes, reinterpret_cast<void**>(&scaleSupport_)) != kResultOk)
                scaleSupport_ = nullptr;
            if (scaleSupport_) scaleSupport_->setContentScaleFactor(scale);
            attached = view_->attached(parent, kPlatformTypeHwnd);
            if (attached == kResultOk) {
                view_->getSize(&r);
                resizable_ = view_->canResize() == kResultTrue;
            }
        });
        if (Crashed()) return Fail(E_FAIL, L"The editor crashed while opening: " + ExceptionName(CrashCode()));
        if (!view_ || attached != kResultOk) {
            if (!view_) noEditor_ = true;
            ReleaseView();
            return Fail(E_FAIL, L"The plug-in has no editor for Windows");
        }
        return SIZE{std::max(r.Width(), 64), std::max(r.Height(), 32)};
    }

    void CloseEditor() override {
        if (!view_) return;
        Call(L"closing the editor", [&] {
            view_->removed();
            view_->setFrame(nullptr);
        });
        ReleaseView();
    }

    [[nodiscard]] bool EditorOpen() const override { return view_ != nullptr; }
    [[nodiscard]] bool EditorResizable() const override { return view_ && resizable_; }
    [[nodiscard]] bool EditorScales() const override { return scaleSupport_ != nullptr; }

    SIZE ConstrainEditorSize(SIZE s) override {
        if (!view_ || !resizable_) return s;
        ViewRect r{0, 0, s.cx, s.cy};
        Call(L"resizing the editor", [&] { view_->checkSizeConstraint(&r); });
        return {std::max(r.Width(), 32), std::max(r.Height(), 32)};
    }

    void SetEditorSize(SIZE s) override {
        if (!view_) return;
        ViewRect r{0, 0, s.cx, s.cy};
        Call(L"resizing the editor", [&] { view_->onSize(&r); });
    }

    void SetEditorScale(float scale) override {
        if (scaleSupport_) Call(L"scaling the editor", [&] { scaleSupport_->setContentScaleFactor(scale); });
    }

    void Idle() override {
        if (!controller_ || Crashed()) return;
        Call(L"parameter update", [&] {
            fromProcessor_.Drain([&](const ParamRing::Item& it) { controller_->setParamNormalized(it.id, it.value); });
        });
    }

    void Process(const float* const* in, float* const* out, uint32_t channels, uint32_t frames) override {
        if (!active_) return;
        inChanges_.Clear();
        toProcessor_.Drain([&](const ParamRing::Item& it) {
            int32_t index = 0;
            if (IParamValueQueue* q = inChanges_.addParameterData(it.id, index)) q->addPoint(0, it.value, index);
        });
        outChanges_.Clear();
        if (!inBuses_.empty()) {
            MapInput(in, channels, inBuses_[0].channelBuffers32, static_cast<uint32_t>(inBuses_[0].numChannels), frames);
            for (size_t b = 1; b < inBuses_.size(); ++b)
                for (int32_t c = 0; c < inBuses_[b].numChannels; ++c) std::fill_n(inBuses_[b].channelBuffers32[c], frames, 0.0f);
        }
        data_.numSamples = static_cast<int32_t>(frames);
        tresult r = kResultOk;
        if (Crash(Guarded([&] { r = processor_->process(data_); }), L"processing")) return;
        MapOutput(outBuses_[0].channelBuffers32, outputs_, in, out, channels, frames);
        for (int32_t i = 0; i < outChanges_.Used(); ++i) {
            const ParamQueue& q = outChanges_.Queue(i);
            if (q.count > 0) fromProcessor_.Push(q.id, q.points[size_t(q.count) - 1].value);
        }
        context_.projectTimeSamples += frames;
        context_.continousTimeSamples += frames;
        context_.projectTimeMusic = double(context_.projectTimeSamples) / context_.sampleRate * context_.tempo / 60.0;
    }

    // ---- Callbacks from the plug-in
    void OnEdit(ParamID id, ParamValue v) {
        toProcessor_.PushLocked(id, v);
        if (callbacks_) callbacks_->StateChanged(*this);
    }
    void OnRestart(int32_t flags) {
        if ((flags & (kLatencyChanged | kIoChanged | kReloadComponent)) && callbacks_ && active_) callbacks_->RestartRequested(*this);
        if (callbacks_) callbacks_->StateChanged(*this);
    }
    void OnDirty() {
        if (callbacks_) callbacks_->StateChanged(*this);
    }
    tresult OnResize(IPlugView* view, ViewRect* r) {
        if (!r || view != view_) return kInvalidArgument;
        if (callbacks_) callbacks_->EditorResizeRequest(*this, r->Width(), r->Height());
        Call(L"resizing the editor", [&] { view->onSize(r); });
        return kResultTrue;
    }

private:
    class Handler final : public IComponentHandler, public IComponentHandler2 {
    public:
        explicit Handler(Vst3Plugin* o) : owner_(o) {}
        tresult DGMOD_VST3_API queryInterface(const char* iid, void** obj) override {
            if (kIidFUnknown.Is(iid) || kIidIComponentHandler.Is(iid)) {
                *obj = static_cast<IComponentHandler*>(this);
                return kResultOk;
            }
            if (kIidIComponentHandler2.Is(iid)) {
                *obj = static_cast<IComponentHandler2*>(this);
                return kResultOk;
            }
            *obj = nullptr;
            return kNoInterface;
        }
        DGMOD_OWNED_REFCOUNT
        tresult DGMOD_VST3_API beginEdit(ParamID) override { return kResultOk; }
        tresult DGMOD_VST3_API performEdit(ParamID id, ParamValue v) override {
            owner_->OnEdit(id, v);
            return kResultOk;
        }
        tresult DGMOD_VST3_API endEdit(ParamID) override { return kResultOk; }
        tresult DGMOD_VST3_API restartComponent(int32_t flags) override {
            owner_->OnRestart(flags);
            return kResultOk;
        }
        tresult DGMOD_VST3_API setDirty(TBool) override {
            owner_->OnDirty();
            return kResultOk;
        }
        tresult DGMOD_VST3_API requestOpenEditor(FIDString) override { return kResultFalse; }
        tresult DGMOD_VST3_API startGroupEdit() override { return kResultOk; }
        tresult DGMOD_VST3_API finishGroupEdit() override { return kResultOk; }

    private:
        Vst3Plugin* owner_;
    };

    class Frame final : public IPlugFrame {
    public:
        explicit Frame(Vst3Plugin* o) : owner_(o) {}
        tresult DGMOD_VST3_API queryInterface(const char* iid, void** obj) override {
            if (kIidFUnknown.Is(iid) || kIidIPlugFrame.Is(iid)) {
                *obj = this;
                return kResultOk;
            }
            *obj = nullptr;
            return kNoInterface;
        }
        DGMOD_OWNED_REFCOUNT
        tresult DGMOD_VST3_API resizeView(IPlugView* view, ViewRect* r) override { return owner_->OnResize(view, r); }

    private:
        Vst3Plugin* owner_;
    };

    template <class F>
    bool Call(const wchar_t* where, F&& f) {
        if (Crashed()) return false;
        return !Crash(Guarded(std::forward<F>(f)), where);
    }

    void ReleaseView() {
        Call(L"closing the editor", [&] {
            if (scaleSupport_) scaleSupport_->release();
            if (view_) view_->release();
        });
        scaleSupport_ = nullptr;
        view_ = nullptr;
        resizable_ = false;
    }

    HMODULE module_ = nullptr;
    IPluginFactory* factory_ = nullptr;
    IComponent* component_ = nullptr;
    IAudioProcessor* processor_ = nullptr;
    IEditController* controller_ = nullptr;
    IConnectionPoint* componentCp_ = nullptr;
    IConnectionPoint* controllerCp_ = nullptr;
    bool separateController_ = false;
    bool float32_ = true;
    IPlugView* view_ = nullptr;
    IPlugViewContentScaleSupport* scaleSupport_ = nullptr;
    bool resizable_ = false, noEditor_ = false;
    HostApplication host_;
    Handler handler_;
    Frame frame_;
    ParamRing toProcessor_, fromProcessor_;
    ParamChanges inChanges_, outChanges_;
    EmptyEvents events_;
    ProcessContext context_{};
    ProcessData data_{};
    std::vector<float> storage_;
    std::vector<float*> ptrs_;
    std::vector<AudioBusBuffers> inBuses_, outBuses_;
};

}  // namespace

Result<std::unique_ptr<PluginInstance>> LoadVst3(const std::wstring& path, const std::wstring& classId, PluginCallbacks* cb) {
    auto p = std::make_unique<Vst3Plugin>(cb);
    if (auto r = p->Init(path, classId); !r) {
        if (p->Crashed()) (void)p.release();  // leave a crashed module alone
        return std::unexpected(r.error());
    }
    return std::unique_ptr<PluginInstance>(std::move(p));
}

void ScanVst3(const std::wstring& path, PluginFileScan& s) {
    std::wstring error;
    HMODULE module = LoadPluginModule(PluginBinaryPath(path), error);
    if (!module) {
        s.error = error;
        return;
    }
    auto getFactory = reinterpret_cast<GetFactoryProc>(::GetProcAddress(module, "GetPluginFactory"));
    if (!getFactory) {
        s.error = L"Not a VST 3 plug-in (no GetPluginFactory)";
        ::FreeLibrary(module);
        return;
    }
    auto initDll = reinterpret_cast<InitModuleProc>(::GetProcAddress(module, "InitDll"));
    auto exitDll = reinterpret_cast<InitModuleProc>(::GetProcAddress(module, "ExitDll"));
    const uint32_t code = Guarded([&] {
        if (initDll && !initDll()) return;
        IPluginFactory* f = getFactory();
        if (!f) return;
        PFactoryInfo fi{};
        f->getFactoryInfo(&fi);
        IPluginFactory2* f2 = nullptr;
        if (f->queryInterface(kIidIPluginFactory2.bytes, reinterpret_cast<void**>(&f2)) != kResultOk) f2 = nullptr;
        const int32_t n = f->countClasses();
        for (int32_t i = 0; i < n; ++i) {
            PClassInfo2 info2{};
            PClassInfo info{};
            const bool two = f2 && f2->getClassInfo2(i, &info2) == kResultOk;
            if (two) {
                std::memcpy(info.cid, info2.cid, 16);
                std::memcpy(info.category, info2.category, sizeof(info.category));
                std::memcpy(info.name, info2.name, sizeof(info.name));
            } else if (f->getClassInfo(i, &info) != kResultOk) {
                continue;
            }
            if (std::strncmp(info.category, kAudioEffectClass, sizeof(info.category)) != 0) continue;
            PluginClassInfo c;
            c.format = PluginFormat::Vst3;
            c.path = path;
            c.classId = FormatClassId(info.cid);
            c.name = FromUtf8(info.name, sizeof(info.name));
            c.vendor = two && info2.vendor[0] ? FromUtf8(info2.vendor, sizeof(info2.vendor)) : FromUtf8(fi.vendor, sizeof(fi.vendor));
            if (two) {
                const std::string sub(info2.subCategories, strnlen(info2.subCategories, sizeof(info2.subCategories)));
                c.category = Widen(sub);
                c.version = FromUtf8(info2.version, sizeof(info2.version));
                c.instrument = sub.find("Instrument") != std::string::npos || sub.find("Generator") != std::string::npos;
            }
            s.classes.push_back(std::move(c));
        }
        if (f2) f2->release();
        f->release();
        if (exitDll) exitDll();
    });
    if (code) {
        s.classes.clear();
        s.error = L"The plug-in crashed while scanning: " + ExceptionName(code);
        return;  // leave the module loaded
    }
    if (s.classes.empty() && s.error.empty()) s.error = L"The plug-in contains no audio effect";
    ::FreeLibrary(module);
}

}  // namespace dgmod::plugins
