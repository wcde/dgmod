// VST 2.4 effect hosting.

#include "plugins/Plugin.h"
#include "plugins/VstAbi.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>

namespace dgmod::plugins {

namespace {

using namespace vst2;

class Vst2Plugin;

// Shell sub-plug-in selection while VSTPluginMain/effOpen run, and whether a shell is being enumerated.
thread_local int32_t t_shellId = 0;
thread_local bool t_scanningShell = false;

intptr_t __cdecl HostCallback(AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);

constexpr uint32_t kStateMagic = 0x32564744;  // "DGV2"
enum : uint32_t { kStateChunk = 1, kStateParams = 2 };

std::wstring FromAnsi(const char* s) {
    const int n = ::MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? size_t(n) - 1 : 0, L'\0');
    if (n > 1) ::MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n);
    return w;
}

// Loads the module and calls its entry point. `shellId` selects a shell's sub-plug-in (0 = none).
AEffect* OpenEffect(HMODULE module, int32_t shellId, std::wstring& error) {
    auto entry = reinterpret_cast<MainProc>(::GetProcAddress(module, "VSTPluginMain"));
    if (!entry) entry = reinterpret_cast<MainProc>(::GetProcAddress(module, "main"));
    if (!entry) {
        error = L"Not a VST 2 plug-in (no VSTPluginMain)";
        return nullptr;
    }
    AEffect* effect = nullptr;
    t_shellId = shellId;
    const uint32_t code = Guarded([&] { effect = entry(HostCallback); });
    t_shellId = 0;
    if (code) {
        error = L"The plug-in crashed while starting: " + ExceptionName(code);
        return nullptr;
    }
    if (!effect || effect->magic != kEffectMagic) {
        error = effect ? L"Not a VST 2 plug-in (bad magic)" : L"The plug-in did not create an effect";
        return nullptr;
    }
    return effect;
}

class Vst2Plugin final : public PluginInstance {
public:
    Vst2Plugin(PluginCallbacks* cb, std::wstring directory) : directory_(Narrow(directory)) {
        callbacks_ = cb;
        format_ = PluginFormat::Vst2;
        std::memset(&time_, 0, sizeof(time_));
    }

    ~Vst2Plugin() override {
        if (Crashed()) return;  // a crashed module is left loaded: its threads may still run
        if (!effect_) {
            if (module_) ::FreeLibrary(module_);
            return;
        }
        CloseEditor();
        Deactivate();
        Dispatch(effClose);
        effect_ = nullptr;
        if (!Crashed() && module_) ::FreeLibrary(module_);
    }

    Result<void> Init(const std::wstring& path, const std::wstring& shellId) {
        std::wstring error;
        module_ = LoadPluginModule(path, error);
        if (!module_) return Fail(E_FAIL, error);
        const int32_t id = shellId.empty() ? 0 : static_cast<int32_t>(std::wcstoul(shellId.c_str(), nullptr, 10));
        effect_ = OpenEffect(module_, id, error);
        if (!effect_) return Fail(E_FAIL, error);
        effect_->resvd1 = reinterpret_cast<intptr_t>(this);
        if (Dispatch(effGetPlugCategory) == kPlugCategShell && !id) return Fail(E_FAIL, L"A shell plug-in: choose one of its effects");
        if (Crashed()) return Fail(E_FAIL, L"The plug-in crashed while opening");
        t_shellId = id;  // some shells ask for the current ID again in effOpen
        Dispatch(effOpen);
        t_shellId = 0;
        if (Crashed()) return Fail(E_FAIL, L"The plug-in crashed while opening: " + ExceptionName(CrashCode()));
        char text[256]{};
        Dispatch(effGetEffectName, 0, 0, text);
        name_ = FromAnsi(text);
        std::memset(text, 0, sizeof(text));
        Dispatch(effGetVendorString, 0, 0, text);
        vendor_ = FromAnsi(text);
        if (name_.empty()) {
            std::memset(text, 0, sizeof(text));
            Dispatch(effGetProductString, 0, 0, text);
            name_ = FromAnsi(text);
        }
        if (name_.empty()) name_ = std::filesystem::path(path).stem().wstring();
        if (!effect_->processReplacing && !effect_->process) return Fail(E_FAIL, L"The plug-in has no process function");
        return {};
    }

    Result<void> Activate(const ProcessFormat& f) override {
        if (Crashed()) return Fail(E_FAIL, L"The plug-in crashed");
        Deactivate();
        processFormat_ = f;
        time_.sampleRate = f.rate;
        Dispatch(effSetSampleRate, 0, 0, nullptr, static_cast<float>(f.rate));
        Dispatch(effSetBlockSize, 0, static_cast<intptr_t>(f.maxFrames));
        Dispatch(effSetProcessPrecision, 0, 0);  // 32-bit float
        Dispatch(effMainsChanged, 0, 1);
        Dispatch(effStartProcess);
        if (Crashed()) return Fail(E_FAIL, L"The plug-in crashed while starting: " + ExceptionName(CrashCode()));
        inputs_ = static_cast<uint32_t>(std::max(0, effect_->numInputs));
        outputs_ = static_cast<uint32_t>(std::max(0, effect_->numOutputs));
        if (outputs_ == 0) return Fail(E_FAIL, L"The plug-in has no audio output");
        latency_ = static_cast<uint32_t>(std::max(0, effect_->initialDelay));
        const uint32_t n = std::max(inputs_, outputs_);
        storage_.assign(size_t(n) * 2 * f.maxFrames, 0.0f);
        inPtr_.resize(std::max(inputs_, 1u));
        outPtr_.resize(outputs_);
        for (uint32_t c = 0; c < inputs_; ++c) inPtr_[c] = storage_.data() + size_t(c) * f.maxFrames;
        if (!inputs_) inPtr_[0] = storage_.data();
        for (uint32_t c = 0; c < outputs_; ++c) outPtr_[c] = storage_.data() + size_t(n + c) * f.maxFrames;
        active_ = true;
        return {};
    }

    void Deactivate() override {
        if (!active_) return;
        active_ = false;
        Dispatch(effStopProcess);
        Dispatch(effMainsChanged, 0, 0);
    }

    std::vector<uint8_t> SaveState() override {
        std::vector<uint8_t> out;
        if (Crashed()) return out;
        auto put = [&](uint32_t v) { out.insert(out.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); };
        put(kStateMagic);
        if (effect_->flags & effFlagsProgramChunks) {
            void* data = nullptr;
            const intptr_t n = Dispatch(effGetChunk, 0, 0, &data);
            if (n > 0 && data && !Crashed()) {
                put(kStateChunk);
                put(static_cast<uint32_t>(Dispatch(effGetProgram)));
                const auto* p = static_cast<const uint8_t*>(data);
                out.insert(out.end(), p, p + n);
                return out;
            }
        }
        put(kStateParams);
        put(static_cast<uint32_t>(Dispatch(effGetProgram)));
        put(static_cast<uint32_t>(std::max(0, effect_->numParams)));
        for (int32_t i = 0; i < effect_->numParams && !Crashed(); ++i) {
            float v = 0;
            Guard(L"getParameter", [&] { v = effect_->getParameter(effect_, i); });
            uint32_t bits;
            std::memcpy(&bits, &v, 4);
            put(bits);
        }
        if (Crashed()) out.clear();
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
        const uint32_t kind = get(4), program = get(8);
        if (kind == kStateChunk) {
            std::vector<uint8_t> chunk(data.begin() + 12, data.end());  // the plug-in may keep or modify it
            Dispatch(effSetProgram, 0, program);
            Dispatch(effSetChunk, 0, static_cast<intptr_t>(chunk.size()), chunk.data());
        } else if (kind == kStateParams) {
            Dispatch(effSetProgram, 0, program);
            const uint32_t n = std::min<uint32_t>(get(12), static_cast<uint32_t>(std::max(0, effect_->numParams)));
            for (uint32_t i = 0; i < n && 16 + size_t(i) * 4 + 4 <= data.size() && !Crashed(); ++i) {
                const uint32_t bits = get(16 + size_t(i) * 4);
                float v;
                std::memcpy(&v, &bits, 4);
                Guard(L"setParameter", [&] { effect_->setParameter(effect_, static_cast<int32_t>(i), v); });
            }
        } else {
            return Fail(E_INVALIDARG, L"Unknown state format");
        }
        if (Crashed()) return Fail(E_FAIL, L"The plug-in crashed while loading its state: " + ExceptionName(CrashCode()));
        return {};
    }

    [[nodiscard]] bool HasEditor() const override { return effect_ && (effect_->flags & effFlagsHasEditor); }

    Result<SIZE> OpenEditor(HWND parent, float) override {
        if (!HasEditor()) return Fail(E_NOTIMPL, L"The plug-in has no editor");
        if (Crashed()) return Fail(E_FAIL, L"The plug-in crashed");
        ERect* r = nullptr;
        Dispatch(effEditGetRect, 0, 0, &r);  // some plug-ins only know their size after opening
        Dispatch(effEditOpen, 0, 0, parent);
        if (Crashed()) return Fail(E_FAIL, L"The editor crashed while opening: " + ExceptionName(CrashCode()));
        editorOpen_ = true;
        r = nullptr;
        Dispatch(effEditGetRect, 0, 0, &r);
        SIZE s{640, 400};
        if (r && r->right > r->left && r->bottom > r->top) s = {r->right - r->left, r->bottom - r->top};
        return s;
    }

    void CloseEditor() override {
        if (!editorOpen_) return;
        editorOpen_ = false;
        Dispatch(effEditClose);
    }

    [[nodiscard]] bool EditorOpen() const override { return editorOpen_; }

    void Idle() override {
        if (editorOpen_) Dispatch(effEditIdle);
    }

    void Process(const float* const* in, float* const* out, uint32_t channels, uint32_t frames) override {
        if (!active_) return;
        renderThread_ = ::GetCurrentThreadId();
        MapInput(in, channels, inPtr_.data(), inputs_, frames);
        if (!inputs_) std::fill_n(inPtr_[0], frames, 0.0f);
        const bool replacing = effect_->processReplacing && (effect_->flags & effFlagsCanReplacing || !effect_->process);
        if (!replacing)
            for (uint32_t c = 0; c < outputs_; ++c) std::fill_n(outPtr_[c], frames, 0.0f);
        const auto fn = replacing ? effect_->processReplacing : effect_->process;
        if (Crash(Guarded([&] { fn(effect_, inPtr_.data(), outPtr_.data(), static_cast<int32_t>(frames)); }), L"processing"))
            return;
        MapOutput(outPtr_.data(), outputs_, in, out, channels, frames);
        time_.samplePos += frames;
        time_.ppqPos = time_.samplePos / time_.sampleRate * time_.tempo / 60.0;
    }

    // ---- Host callback
    // Host callback opcodes that depend on the instance; false = not handled here.
    bool OnHost(int32_t opcode, int32_t index, intptr_t value, intptr_t& result) {
        result = 1;
        switch (opcode) {
            case audioMasterAutomate:
            case audioMasterBeginEdit:
            case audioMasterEndEdit:
                if (callbacks_) callbacks_->StateChanged(*this);
                result = opcode == audioMasterAutomate ? 0 : 1;
                return true;
            case audioMasterGetTime: {
                time_.tempo = 120.0;
                time_.timeSigNumerator = 4;
                time_.timeSigDenominator = 4;
                time_.flags = VstTimeInfo::kVstTransportPlaying | VstTimeInfo::kVstPpqPosValid | VstTimeInfo::kVstTempoValid |
                              VstTimeInfo::kVstTimeSigValid;
                if (time_.sampleRate <= 0) time_.sampleRate = processFormat_.rate > 0 ? processFormat_.rate : 48000.0;
                result = reinterpret_cast<intptr_t>(&time_);
                return true;
            }
            case audioMasterIOChanged:
                if (callbacks_ && active_) callbacks_->RestartRequested(*this);
                return true;
            case audioMasterSizeWindow:
                if (callbacks_ && editorOpen_) callbacks_->EditorResizeRequest(*this, index, static_cast<int>(value));
                return true;
            case audioMasterGetSampleRate:
                result = static_cast<intptr_t>(processFormat_.rate > 0 ? processFormat_.rate : 48000.0);
                return true;
            case audioMasterGetBlockSize:
                result = processFormat_.maxFrames ? processFormat_.maxFrames : 512;
                return true;
            case audioMasterGetCurrentProcessLevel:
                result = ::GetCurrentThreadId() == renderThread_ ? kVstProcessLevelRealtime : kVstProcessLevelUser;
                return true;
            case audioMasterGetDirectory:
                result = reinterpret_cast<intptr_t>(directory_.c_str());
                return true;
            case audioMasterUpdateDisplay: return true;
            default: return false;
        }
    }

private:
    template <class F>
    void Guard(const wchar_t* where, F&& f) {
        if (!Crashed()) Crash(Guarded(std::forward<F>(f)), where);
    }

    intptr_t Dispatch(int32_t opcode, int32_t index = 0, intptr_t value = 0, void* ptr = nullptr, float opt = 0.0f) {
        intptr_t r = 0;
        if (!effect_ || Crashed()) return 0;
        Crash(Guarded([&] { r = effect_->dispatcher(effect_, opcode, index, value, ptr, opt); }), L"dispatcher");
        return r;
    }

    HMODULE module_ = nullptr;
    AEffect* effect_ = nullptr;
    std::string directory_;
    VstTimeInfo time_;
    bool editorOpen_ = false;
    std::atomic<DWORD> renderThread_{0};
    std::vector<float> storage_;
    std::vector<float*> inPtr_, outPtr_;
};

intptr_t __cdecl HostCallback(AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float) {
    if (effect && effect->resvd1) {
        intptr_t result = 0;
        if (reinterpret_cast<Vst2Plugin*>(effect->resvd1)->OnHost(opcode, index, value, result)) return result;
    }
    switch (opcode) {
        case audioMasterVersion: return 2400;
        case audioMasterCurrentId: return t_shellId ? t_shellId : (effect ? effect->uniqueID : 0);
        case audioMasterGetVendorString:
            if (ptr) std::memcpy(ptr, "dgmod", 6);
            return 1;
        case audioMasterGetProductString:
            if (ptr) std::memcpy(ptr, "dgmod", 6);
            return 1;
        case audioMasterGetVendorVersion: return 1000;
        case audioMasterGetLanguage: return 1;  // English
        case audioMasterGetAutomationState: return 1;  // off
        case audioMasterGetSampleRate: return 48000;
        case audioMasterGetBlockSize: return 512;
        case audioMasterCanDo: {
            const char* what = static_cast<const char*>(ptr);
            if (!what) return 0;
            for (const char* yes : {"sendVstTimeInfo", "sizeWindow", "startStopProcess", "supplyIdle", "acceptIOChanges"})
                if (std::strcmp(what, yes) == 0) return 1;
            if (std::strcmp(what, "shellCategory") == 0) return t_scanningShell ? 1 : 0;
            return 0;
        }
        default: return 0;
    }
}

}  // namespace

Result<std::unique_ptr<PluginInstance>> LoadVst2(const std::wstring& path, const std::wstring& shellId, PluginCallbacks* cb) {
    auto p = std::make_unique<Vst2Plugin>(cb, std::filesystem::path(path).parent_path().wstring());
    if (auto r = p->Init(path, shellId); !r) {
        if (p->Crashed()) (void)p.release();  // leave a crashed module alone
        return std::unexpected(r.error());
    }
    return std::unique_ptr<PluginInstance>(std::move(p));
}

void ScanVst2(const std::wstring& path, PluginFileScan& s) {
    std::wstring error;
    HMODULE module = LoadPluginModule(path, error);
    if (!module) {
        s.error = error;
        return;
    }
    if (!::GetProcAddress(module, "VSTPluginMain") && !::GetProcAddress(module, "main")) {
        s.notPlugin = true;
        ::FreeLibrary(module);
        return;
    }
    t_scanningShell = true;
    AEffect* effect = OpenEffect(module, 0, error);
    t_scanningShell = false;
    if (!effect) {
        s.error = error;
        return;
    }
    auto dispatch = [&](int32_t op, int32_t index = 0, intptr_t value = 0, void* ptr = nullptr) {
        intptr_t r = 0;
        const uint32_t code = Guarded([&] { r = effect->dispatcher(effect, op, index, value, ptr, 0.0f); });
        if (code) throw code;
        return r;
    };
    try {
        const intptr_t category = dispatch(effGetPlugCategory);
        char vendor[256]{};
        dispatch(effGetVendorString, 0, 0, vendor);
        if (category == kPlugCategShell) {
            for (int i = 0; i < 1000; ++i) {
                char name[256]{};
                const intptr_t id = dispatch(effShellGetNextPlugin, 0, 0, name);
                if (!id || !name[0]) break;
                PluginClassInfo c;
                c.format = PluginFormat::Vst2;
                c.path = path;
                c.classId = std::to_wstring(static_cast<int32_t>(id));
                c.name = FromAnsi(name);
                c.vendor = FromAnsi(vendor);
                c.category = L"Shell";
                s.classes.push_back(std::move(c));
            }
        } else {
            dispatch(effOpen);
            char name[256]{};
            dispatch(effGetEffectName, 0, 0, name);
            if (!name[0]) dispatch(effGetProductString, 0, 0, name);
            PluginClassInfo c;
            c.format = PluginFormat::Vst2;
            c.path = path;
            c.name = name[0] ? FromAnsi(name) : std::filesystem::path(path).stem().wstring();
            c.vendor = FromAnsi(vendor);
            c.instrument = (effect->flags & effFlagsIsSynth) || category == kPlugCategSynth || category == kPlugCategGenerator ||
                           effect->numOutputs == 0;
            static const wchar_t* kCategories[] = {L"Effect",       L"Effect",       L"Instrument", L"Analyzer",
                                                   L"Mastering",    L"Spatial",      L"Reverb",     L"Surround",
                                                   L"Restoration",  L"Offline",      L"Shell",      L"Generator"};
            c.category = category >= 0 && category < 12 ? kCategories[category] : L"Effect";
            const intptr_t version = dispatch(effGetVendorVersion);
            if (version > 0) c.version = std::to_wstring(version);
            s.classes.push_back(std::move(c));
            dispatch(effClose);
        }
    } catch (uint32_t code) {
        s.classes.clear();
        s.error = L"The plug-in crashed while scanning: " + ExceptionName(code);
        return;  // leave the module loaded
    }
    ::FreeLibrary(module);
}

}  // namespace dgmod::plugins
