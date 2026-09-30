#pragma once

// Binary interfaces of the two plug-in formats dgmod hosts, declared from their public ABI (no SDK needed):
//  * VST 3: COM-compatible interfaces (vtable layout, 16-byte interface IDs, tresult = HRESULT on Windows);
//  * VST 2.4: the AEffect structure and its dispatcher/host callback opcodes.
// Only what an effect host uses is declared. Structures use the platform's natural alignment (8 on x64), as the
// formats specify.

#include <cstdint>
#include <cstring>

namespace dgmod::plugins {

// =====================================================================================================================
// VST 3
// =====================================================================================================================
namespace vst3 {

#define DGMOD_VST3_API __stdcall

using tresult = int32_t;
using TBool = uint8_t;
using char16 = char16_t;
using String128 = char16[128];
using ParamID = uint32_t;
using ParamValue = double;
using SpeakerArrangement = uint64_t;
using FIDString = const char*;

inline constexpr tresult kResultOk = 0;
inline constexpr tresult kResultTrue = 0;
inline constexpr tresult kResultFalse = 1;
inline constexpr tresult kNoInterface = static_cast<tresult>(0x80004002L);
inline constexpr tresult kInvalidArgument = static_cast<tresult>(0x80070057L);
inline constexpr tresult kNotImplemented = static_cast<tresult>(0x80004001L);

// Interface/class ID in the COM-compatible byte order used on Windows (the first 32 bits and the two 16-bit halves of
// the second are little-endian, the last 64 bits big-endian): the layout of a Windows GUID.
struct Uid {
    char bytes[16];
    [[nodiscard]] bool Is(const char* other) const { return other && std::memcmp(bytes, other, 16) == 0; }
};

constexpr Uid MakeUid(uint32_t l1, uint32_t l2, uint32_t l3, uint32_t l4) {
    auto b = [](uint32_t v, int shift) { return static_cast<char>((v >> shift) & 0xFF); };
    return {{b(l1, 0), b(l1, 8), b(l1, 16), b(l1, 24), b(l2, 16), b(l2, 24), b(l2, 0), b(l2, 8), b(l3, 24), b(l3, 16),
             b(l3, 8), b(l3, 0), b(l4, 24), b(l4, 16), b(l4, 8), b(l4, 0)}};
}

inline constexpr Uid kIidFUnknown = MakeUid(0x00000000, 0x00000000, 0xC0000000, 0x00000046);
inline constexpr Uid kIidIPluginBase = MakeUid(0x22888DDB, 0x156E45AE, 0x8358B348, 0x08190625);
inline constexpr Uid kIidIPluginFactory = MakeUid(0x7A4D811C, 0x52114A1F, 0xAED9D2EE, 0x0B43BF9F);
inline constexpr Uid kIidIPluginFactory2 = MakeUid(0x0007B650, 0xF24B4C0B, 0xA464EDB9, 0xF00B2ABB);
inline constexpr Uid kIidIComponent = MakeUid(0xE831FF31, 0xF2D54301, 0x928EBBEE, 0x25697802);
inline constexpr Uid kIidIAudioProcessor = MakeUid(0x42043F99, 0xB7DA453C, 0xA569E79D, 0x9AAEC33D);
inline constexpr Uid kIidIEditController = MakeUid(0xDCD7BBE3, 0x7742448D, 0xA874AACC, 0x979C759E);
inline constexpr Uid kIidIConnectionPoint = MakeUid(0x70A4156F, 0x6E6E4026, 0x989148BF, 0xAA60D8D1);
inline constexpr Uid kIidIBStream = MakeUid(0xC3BF6EA2, 0x30994752, 0x9B6BF990, 0x1EE33E9B);
inline constexpr Uid kIidIPlugView = MakeUid(0x5BC32507, 0xD06049EA, 0xA6151B52, 0x2B755B29);
inline constexpr Uid kIidIPlugFrame = MakeUid(0x367FAF01, 0xAFA94693, 0x8D4DA2A0, 0xED0882A3);
inline constexpr Uid kIidIPlugViewContentScaleSupport = MakeUid(0x65ED9690, 0x8AC44525, 0x8AADEF7A, 0x72EA703F);
inline constexpr Uid kIidIHostApplication = MakeUid(0x58E595CC, 0xDB2D4969, 0x8B6AAF8C, 0x36A664E5);
inline constexpr Uid kIidIComponentHandler = MakeUid(0x93A0BEA3, 0x0BD045DB, 0x8E890B0C, 0xC1E46AC6);
inline constexpr Uid kIidIComponentHandler2 = MakeUid(0xF040B4B3, 0xA36045EC, 0xABCDC045, 0xB4D5A2CC);
inline constexpr Uid kIidIMessage = MakeUid(0x936F033B, 0xC6C047DB, 0xBB0882F8, 0x13C1E613);
inline constexpr Uid kIidIAttributeList = MakeUid(0x1E5F0AEB, 0xCC7F4533, 0xA2544011, 0x38AD5EE4);
inline constexpr Uid kIidIParameterChanges = MakeUid(0xA4779663, 0x0BB64A56, 0xB44384A8, 0x466FEB9D);
inline constexpr Uid kIidIParamValueQueue = MakeUid(0x01263A18, 0xED074F6F, 0x98C9D356, 0x4686F9BA);
inline constexpr Uid kIidIEventList = MakeUid(0x3A2C4214, 0x346349FE, 0xB2C4F397, 0xB9695A44);

inline constexpr char kAudioEffectClass[] = "Audio Module Class";
inline constexpr char kPlatformTypeHwnd[] = "HWND";
inline constexpr char kEditorView[] = "editor";

struct FUnknown {
    virtual tresult DGMOD_VST3_API queryInterface(const char* iid, void** obj) = 0;
    virtual uint32_t DGMOD_VST3_API addRef() = 0;
    virtual uint32_t DGMOD_VST3_API release() = 0;
};

struct IPluginBase : FUnknown {
    virtual tresult DGMOD_VST3_API initialize(FUnknown* context) = 0;
    virtual tresult DGMOD_VST3_API terminate() = 0;
};

struct PFactoryInfo {
    char vendor[64];
    char url[256];
    char email[128];
    int32_t flags;
};

struct PClassInfo {
    char cid[16];
    int32_t cardinality;
    char category[32];
    char name[64];
};

struct PClassInfo2 {
    char cid[16];
    int32_t cardinality;
    char category[32];
    char name[64];
    uint32_t classFlags;
    char subCategories[128];
    char vendor[64];
    char version[64];
    char sdkVersion[64];
};

struct IPluginFactory : FUnknown {
    virtual tresult DGMOD_VST3_API getFactoryInfo(PFactoryInfo* info) = 0;
    virtual int32_t DGMOD_VST3_API countClasses() = 0;
    virtual tresult DGMOD_VST3_API getClassInfo(int32_t index, PClassInfo* info) = 0;
    virtual tresult DGMOD_VST3_API createInstance(FIDString cid, FIDString iid, void** obj) = 0;
};

struct IPluginFactory2 : IPluginFactory {
    virtual tresult DGMOD_VST3_API getClassInfo2(int32_t index, PClassInfo2* info) = 0;
};

enum MediaTypes : int32_t { kAudio = 0, kEvent = 1 };
enum BusDirections : int32_t { kInput = 0, kOutput = 1 };
enum BusTypes : int32_t { kMain = 0, kAux = 1 };

struct BusInfo {
    int32_t mediaType;
    int32_t direction;
    int32_t channelCount;
    String128 name;
    int32_t busType;
    uint32_t flags;
};

struct RoutingInfo {
    int32_t mediaType;
    int32_t busIndex;
    int32_t channel;
};

struct IBStream;

struct IComponent : IPluginBase {
    virtual tresult DGMOD_VST3_API getControllerClassId(char classId[16]) = 0;
    virtual tresult DGMOD_VST3_API setIoMode(int32_t mode) = 0;
    virtual int32_t DGMOD_VST3_API getBusCount(int32_t type, int32_t dir) = 0;
    virtual tresult DGMOD_VST3_API getBusInfo(int32_t type, int32_t dir, int32_t index, BusInfo& bus) = 0;
    virtual tresult DGMOD_VST3_API getRoutingInfo(RoutingInfo& inInfo, RoutingInfo& outInfo) = 0;
    virtual tresult DGMOD_VST3_API activateBus(int32_t type, int32_t dir, int32_t index, TBool state) = 0;
    virtual tresult DGMOD_VST3_API setActive(TBool state) = 0;
    virtual tresult DGMOD_VST3_API setState(IBStream* state) = 0;
    virtual tresult DGMOD_VST3_API getState(IBStream* state) = 0;
};

inline constexpr SpeakerArrangement kSpeakerArrMono = 1ull << 19;  // kSpeakerM
inline constexpr SpeakerArrangement kSpeakerArrStereo = 0x3;       // kSpeakerL | kSpeakerR

enum ProcessModes : int32_t { kRealtime = 0, kPrefetch = 1, kOffline = 2 };
enum SymbolicSampleSizes : int32_t { kSample32 = 0, kSample64 = 1 };

struct ProcessSetup {
    int32_t processMode;
    int32_t symbolicSampleSize;
    int32_t maxSamplesPerBlock;
    double sampleRate;
};

struct AudioBusBuffers {
    int32_t numChannels;
    uint64_t silenceFlags;
    float** channelBuffers32;  // union with double** channelBuffers64
};

struct IParamValueQueue : FUnknown {
    virtual ParamID DGMOD_VST3_API getParameterId() = 0;
    virtual int32_t DGMOD_VST3_API getPointCount() = 0;
    virtual tresult DGMOD_VST3_API getPoint(int32_t index, int32_t& sampleOffset, ParamValue& value) = 0;
    virtual tresult DGMOD_VST3_API addPoint(int32_t sampleOffset, ParamValue value, int32_t& index) = 0;
};

struct IParameterChanges : FUnknown {
    virtual int32_t DGMOD_VST3_API getParameterCount() = 0;
    virtual IParamValueQueue* DGMOD_VST3_API getParameterData(int32_t index) = 0;
    virtual IParamValueQueue* DGMOD_VST3_API addParameterData(const ParamID& id, int32_t& index) = 0;
};

struct IEventList : FUnknown {
    virtual int32_t DGMOD_VST3_API getEventCount() = 0;
    virtual tresult DGMOD_VST3_API getEvent(int32_t index, void* e) = 0;  // Event& (never read: no events are sent)
    virtual tresult DGMOD_VST3_API addEvent(void* e) = 0;
};

struct Chord {
    uint8_t keyNote;
    uint8_t rootNote;
    int16_t chordMask;
};

struct FrameRate {
    uint32_t framesPerSecond;
    uint32_t flags;
};

struct ProcessContext {
    enum StatesAndFlags : uint32_t {
        kPlaying = 1 << 1,
        kSystemTimeValid = 1 << 8,
        kProjectTimeMusicValid = 1 << 9,
        kTempoValid = 1 << 10,
        kBarPositionValid = 1 << 11,
        kTimeSigValid = 1 << 13,
        kContTimeValid = 1 << 17,
    };
    uint32_t state;
    double sampleRate;
    int64_t projectTimeSamples;
    int64_t systemTime;
    int64_t continousTimeSamples;
    double projectTimeMusic;
    double barPositionMusic;
    double cycleStartMusic;
    double cycleEndMusic;
    double tempo;
    int32_t timeSigNumerator;
    int32_t timeSigDenominator;
    Chord chord;
    int32_t smpteOffsetSubframes;
    FrameRate frameRate;
    int32_t samplesToNextClock;
};

struct ProcessData {
    int32_t processMode;
    int32_t symbolicSampleSize;
    int32_t numSamples;
    int32_t numInputs;
    int32_t numOutputs;
    AudioBusBuffers* inputs;
    AudioBusBuffers* outputs;
    IParameterChanges* inputParameterChanges;
    IParameterChanges* outputParameterChanges;
    IEventList* inputEvents;
    IEventList* outputEvents;
    ProcessContext* processContext;
};

struct IAudioProcessor : FUnknown {
    virtual tresult DGMOD_VST3_API setBusArrangements(SpeakerArrangement* inputs, int32_t numIns, SpeakerArrangement* outputs,
                                                      int32_t numOuts) = 0;
    virtual tresult DGMOD_VST3_API getBusArrangement(int32_t dir, int32_t index, SpeakerArrangement& arr) = 0;
    virtual tresult DGMOD_VST3_API canProcessSampleSize(int32_t symbolicSampleSize) = 0;
    virtual uint32_t DGMOD_VST3_API getLatencySamples() = 0;
    virtual tresult DGMOD_VST3_API setupProcessing(ProcessSetup& setup) = 0;
    virtual tresult DGMOD_VST3_API setProcessing(TBool state) = 0;
    virtual tresult DGMOD_VST3_API process(ProcessData& data) = 0;
    virtual uint32_t DGMOD_VST3_API getTailSamples() = 0;
};

struct IBStream : FUnknown {
    enum IStreamSeekMode : int32_t { kIBSeekSet = 0, kIBSeekCur = 1, kIBSeekEnd = 2 };
    virtual tresult DGMOD_VST3_API read(void* buffer, int32_t numBytes, int32_t* numBytesRead) = 0;
    virtual tresult DGMOD_VST3_API write(void* buffer, int32_t numBytes, int32_t* numBytesWritten) = 0;
    virtual tresult DGMOD_VST3_API seek(int64_t pos, int32_t mode, int64_t* result) = 0;
    virtual tresult DGMOD_VST3_API tell(int64_t* pos) = 0;
};

struct ParameterInfo {
    ParamID id;
    String128 title;
    String128 shortTitle;
    String128 units;
    int32_t stepCount;
    ParamValue defaultNormalizedValue;
    int32_t unitId;
    int32_t flags;
};

struct IComponentHandler : FUnknown {
    virtual tresult DGMOD_VST3_API beginEdit(ParamID id) = 0;
    virtual tresult DGMOD_VST3_API performEdit(ParamID id, ParamValue valueNormalized) = 0;
    virtual tresult DGMOD_VST3_API endEdit(ParamID id) = 0;
    virtual tresult DGMOD_VST3_API restartComponent(int32_t flags) = 0;
};

struct IComponentHandler2 : FUnknown {
    virtual tresult DGMOD_VST3_API setDirty(TBool state) = 0;
    virtual tresult DGMOD_VST3_API requestOpenEditor(FIDString name) = 0;
    virtual tresult DGMOD_VST3_API startGroupEdit() = 0;
    virtual tresult DGMOD_VST3_API finishGroupEdit() = 0;
};

enum RestartFlags : int32_t {
    kReloadComponent = 1 << 0,
    kIoChanged = 1 << 1,
    kParamValuesChanged = 1 << 2,
    kLatencyChanged = 1 << 3,
};

struct ViewRect {
    int32_t left, top, right, bottom;
    [[nodiscard]] int32_t Width() const { return right - left; }
    [[nodiscard]] int32_t Height() const { return bottom - top; }
};

struct IPlugFrame;

struct IPlugView : FUnknown {
    virtual tresult DGMOD_VST3_API isPlatformTypeSupported(FIDString type) = 0;
    virtual tresult DGMOD_VST3_API attached(void* parent, FIDString type) = 0;
    virtual tresult DGMOD_VST3_API removed() = 0;
    virtual tresult DGMOD_VST3_API onWheel(float distance) = 0;
    virtual tresult DGMOD_VST3_API onKeyDown(char16 key, int16_t keyCode, int16_t modifiers) = 0;
    virtual tresult DGMOD_VST3_API onKeyUp(char16 key, int16_t keyCode, int16_t modifiers) = 0;
    virtual tresult DGMOD_VST3_API getSize(ViewRect* size) = 0;
    virtual tresult DGMOD_VST3_API onSize(ViewRect* newSize) = 0;
    virtual tresult DGMOD_VST3_API onFocus(TBool state) = 0;
    virtual tresult DGMOD_VST3_API setFrame(IPlugFrame* frame) = 0;
    virtual tresult DGMOD_VST3_API canResize() = 0;
    virtual tresult DGMOD_VST3_API checkSizeConstraint(ViewRect* rect) = 0;
};

struct IPlugFrame : FUnknown {
    virtual tresult DGMOD_VST3_API resizeView(IPlugView* view, ViewRect* newSize) = 0;
};

struct IPlugViewContentScaleSupport : FUnknown {
    virtual tresult DGMOD_VST3_API setContentScaleFactor(float factor) = 0;
};

struct IEditController : IPluginBase {
    virtual tresult DGMOD_VST3_API setComponentState(IBStream* state) = 0;
    virtual tresult DGMOD_VST3_API setState(IBStream* state) = 0;
    virtual tresult DGMOD_VST3_API getState(IBStream* state) = 0;
    virtual int32_t DGMOD_VST3_API getParameterCount() = 0;
    virtual tresult DGMOD_VST3_API getParameterInfo(int32_t paramIndex, ParameterInfo& info) = 0;
    virtual tresult DGMOD_VST3_API getParamStringByValue(ParamID id, ParamValue valueNormalized, String128 string) = 0;
    virtual tresult DGMOD_VST3_API getParamValueByString(ParamID id, char16* string, ParamValue& valueNormalized) = 0;
    virtual ParamValue DGMOD_VST3_API normalizedParamToPlain(ParamID id, ParamValue valueNormalized) = 0;
    virtual ParamValue DGMOD_VST3_API plainParamToNormalized(ParamID id, ParamValue plainValue) = 0;
    virtual ParamValue DGMOD_VST3_API getParamNormalized(ParamID id) = 0;
    virtual tresult DGMOD_VST3_API setParamNormalized(ParamID id, ParamValue value) = 0;
    virtual tresult DGMOD_VST3_API setComponentHandler(IComponentHandler* handler) = 0;
    virtual IPlugView* DGMOD_VST3_API createView(FIDString name) = 0;
};

struct IAttributeList : FUnknown {
    virtual tresult DGMOD_VST3_API setInt(const char* id, int64_t value) = 0;
    virtual tresult DGMOD_VST3_API getInt(const char* id, int64_t& value) = 0;
    virtual tresult DGMOD_VST3_API setFloat(const char* id, double value) = 0;
    virtual tresult DGMOD_VST3_API getFloat(const char* id, double& value) = 0;
    virtual tresult DGMOD_VST3_API setString(const char* id, const char16* string) = 0;
    virtual tresult DGMOD_VST3_API getString(const char* id, char16* string, uint32_t sizeInBytes) = 0;
    virtual tresult DGMOD_VST3_API setBinary(const char* id, const void* data, uint32_t sizeInBytes) = 0;
    virtual tresult DGMOD_VST3_API getBinary(const char* id, const void*& data, uint32_t& sizeInBytes) = 0;
};

struct IMessage : FUnknown {
    virtual FIDString DGMOD_VST3_API getMessageID() = 0;
    virtual void DGMOD_VST3_API setMessageID(FIDString id) = 0;
    virtual IAttributeList* DGMOD_VST3_API getAttributes() = 0;
};

struct IConnectionPoint : FUnknown {
    virtual tresult DGMOD_VST3_API connect(IConnectionPoint* other) = 0;
    virtual tresult DGMOD_VST3_API disconnect(IConnectionPoint* other) = 0;
    virtual tresult DGMOD_VST3_API notify(IMessage* message) = 0;
};

struct IHostApplication : FUnknown {
    virtual tresult DGMOD_VST3_API getName(String128 name) = 0;
    virtual tresult DGMOD_VST3_API createInstance(char cid[16], char iid[16], void** obj) = 0;
};

using GetFactoryProc = IPluginFactory*(DGMOD_VST3_API*)();
using InitModuleProc = bool(DGMOD_VST3_API*)();

}  // namespace vst3

// =====================================================================================================================
// VST 2.4
// =====================================================================================================================
namespace vst2 {

struct AEffect;
using HostCallback = intptr_t(__cdecl*)(AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
using DispatcherProc = intptr_t(__cdecl*)(AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
using ProcessProc = void(__cdecl*)(AEffect* effect, float** inputs, float** outputs, int32_t frames);
using ProcessDoubleProc = void(__cdecl*)(AEffect* effect, double** inputs, double** outputs, int32_t frames);
using SetParameterProc = void(__cdecl*)(AEffect* effect, int32_t index, float value);
using GetParameterProc = float(__cdecl*)(AEffect* effect, int32_t index);
using MainProc = AEffect*(__cdecl*)(HostCallback host);

inline constexpr int32_t kEffectMagic = 0x56737450;  // 'VstP'

struct AEffect {
    int32_t magic;
    DispatcherProc dispatcher;
    ProcessProc process;  // deprecated accumulating process
    SetParameterProc setParameter;
    GetParameterProc getParameter;
    int32_t numPrograms;
    int32_t numParams;
    int32_t numInputs;
    int32_t numOutputs;
    int32_t flags;
    intptr_t resvd1;  // reserved for the host: dgmod keeps its instance pointer here
    intptr_t resvd2;
    int32_t initialDelay;
    int32_t realQualities;
    int32_t offQualities;
    float ioRatio;
    void* object;
    void* user;
    int32_t uniqueID;
    int32_t version;
    ProcessProc processReplacing;
    ProcessDoubleProc processDoubleReplacing;
    char future[56];
};

enum EffectFlags : int32_t {
    effFlagsHasEditor = 1 << 0,
    effFlagsCanReplacing = 1 << 4,
    effFlagsProgramChunks = 1 << 5,
    effFlagsIsSynth = 1 << 8,
    effFlagsNoSoundInStop = 1 << 9,
};

enum EffectOpcodes : int32_t {
    effOpen = 0,
    effClose = 1,
    effSetProgram = 2,
    effGetProgram = 3,
    effGetParamName = 8,
    effSetSampleRate = 10,
    effSetBlockSize = 11,
    effMainsChanged = 12,
    effEditGetRect = 13,
    effEditOpen = 14,
    effEditClose = 15,
    effEditIdle = 19,
    effGetChunk = 23,
    effSetChunk = 24,
    effGetPlugCategory = 35,
    effSetSpeakerArrangement = 42,
    effGetEffectName = 45,
    effGetVendorString = 47,
    effGetProductString = 48,
    effGetVendorVersion = 49,
    effCanDo = 51,
    effGetTailSize = 52,
    effGetVstVersion = 58,
    effShellGetNextPlugin = 70,
    effStartProcess = 71,
    effStopProcess = 72,
    effSetProcessPrecision = 77,
};

enum HostOpcodes : int32_t {
    audioMasterAutomate = 0,
    audioMasterVersion = 1,
    audioMasterCurrentId = 2,
    audioMasterIdle = 3,
    audioMasterGetTime = 7,
    audioMasterProcessEvents = 8,
    audioMasterIOChanged = 13,
    audioMasterSizeWindow = 15,
    audioMasterGetSampleRate = 16,
    audioMasterGetBlockSize = 17,
    audioMasterGetInputLatency = 18,
    audioMasterGetOutputLatency = 19,
    audioMasterGetCurrentProcessLevel = 23,
    audioMasterGetAutomationState = 24,
    audioMasterGetVendorString = 32,
    audioMasterGetProductString = 33,
    audioMasterGetVendorVersion = 34,
    audioMasterVendorSpecific = 35,
    audioMasterCanDo = 37,
    audioMasterGetLanguage = 38,
    audioMasterGetDirectory = 41,
    audioMasterUpdateDisplay = 42,
    audioMasterBeginEdit = 43,
    audioMasterEndEdit = 44,
};

enum PlugCategory : int32_t {
    kPlugCategUnknown = 0,
    kPlugCategEffect = 1,
    kPlugCategSynth = 2,
    kPlugCategAnalysis = 3,
    kPlugCategMastering = 4,
    kPlugCategSpacializer = 5,
    kPlugCategRoomFx = 6,
    kPlugSurroundFx = 7,
    kPlugCategRestoration = 8,
    kPlugCategOfflineProcess = 9,
    kPlugCategShell = 10,
    kPlugCategGenerator = 11,
};

enum ProcessLevel : int32_t { kVstProcessLevelUser = 1, kVstProcessLevelRealtime = 2 };

struct ERect {
    int16_t top, left, bottom, right;
};

struct VstTimeInfo {
    enum Flags : int32_t {
        kVstTransportPlaying = 1 << 1,
        kVstNanosValid = 1 << 8,
        kVstPpqPosValid = 1 << 9,
        kVstTempoValid = 1 << 10,
        kVstBarsValid = 1 << 11,
        kVstTimeSigValid = 1 << 13,
    };
    double samplePos;
    double sampleRate;
    double nanoSeconds;
    double ppqPos;
    double tempo;
    double barStartPos;
    double cycleStartPos;
    double cycleEndPos;
    int32_t timeSigNumerator;
    int32_t timeSigDenominator;
    int32_t smpteOffset;
    int32_t smpteFrameRate;
    int32_t samplesToNextClock;
    int32_t flags;
};

}  // namespace vst2

}  // namespace dgmod::plugins
