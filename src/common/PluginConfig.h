#pragma once

// Plug-in chain of the bridge: VST 3 and VST 2 effects run at the source rate, after the tone stages and before the
// oversampling filter. The chain is stored in HKCU\Software\dgmod\Plugins (the bridge watches it and applies changes
// without restarting the session); every plug-in's own state lives in %LOCALAPPDATA%\dgmod\plugins\<id>.state.
// The installed plug-ins are catalogued by scanning each file in a separate process (dgmod-bridge.exe --scan-plugin),
// so a plug-in that crashes while loading only takes the scanner down.

#include "common/Win.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dgmod {

enum class PluginFormat : uint32_t { Vst3 = 0, Vst2 = 1 };

inline constexpr wchar_t kRegPlugins[] = L"Software\\dgmod\\Plugins";
// Message-only window of the bridge's plug-in host; the GUI posts it editor requests (wParam = plug-in id).
inline constexpr wchar_t kPluginHostWindowClass[] = L"dgmodPluginHost";
inline constexpr UINT kPluginHostOpenEditor = WM_APP + 1;
inline constexpr UINT kPluginHostCloseEditor = WM_APP + 2;
inline constexpr UINT kPluginHostReload = WM_APP + 3;  // unload and load again (after a crash)
inline constexpr size_t kMaxPlugins = 16;

struct PluginEntry {
    uint32_t id = 0;  // unique within the chain, never reused while the entry exists; names the state file
    PluginFormat format = PluginFormat::Vst3;
    std::wstring path;     // .vst3 file or bundle folder, or a VST 2 .dll
    std::wstring classId;  // VST 3: 32 hex digits of the class ID; VST 2: shell sub-plug-in ID (decimal) or empty
    std::wstring name, vendor;
    bool enabled = true;       // off = bypassed (still processed, so switching it back is seamless)
    bool quarantined = false;  // crashed the bridge while loading: not loaded until the user retries
    // The editor window is scaled by Windows (bitmap) instead of drawing at the monitor's DPI itself: for editors that
    // come out tiny on a high-DPI display. VST 3 editors without content scaling get this automatically.
    bool windowsScaling = false;
    friend bool operator==(const PluginEntry&, const PluginEntry&) = default;
};

struct PluginChainConfig {
    bool enabled = true;  // master switch: off = no plug-in is processed
    std::vector<PluginEntry> plugins;
    [[nodiscard]] uint32_t NextId() const;
    [[nodiscard]] const PluginEntry* Find(uint32_t id) const;
    friend bool operator==(const PluginChainConfig&, const PluginChainConfig&) = default;
};

// `key` (under HKCU): another chain for tests.
PluginChainConfig LoadPluginChain(const wchar_t* key = kRegPlugins);
HRESULT SavePluginChain(const PluginChainConfig& c, const wchar_t* key = kRegPlugins);

// %LOCALAPPDATA%\dgmod\plugins (created if missing) and the state file of a chain entry.
std::wstring PluginDataDirectory();
std::wstring PluginStatePath(uint32_t id);

// ---- Catalogue of installed plug-ins -------------------------------------------------------------------------------

struct PluginClassInfo {
    PluginFormat format = PluginFormat::Vst3;
    std::wstring path, classId, name, vendor, category, version;
    bool instrument = false;  // an instrument/generator (not offered: the bridge has no MIDI)
};

struct PluginFileScan {
    std::wstring path;
    PluginFormat format = PluginFormat::Vst3;
    uint64_t writeTime = 0, size = 0;  // identify the file version the result belongs to
    std::vector<PluginClassInfo> classes;
    std::wstring error;  // empty on success
    bool notPlugin = false;  // a DLL in a plug-in folder that is not a plug-in (helper library): not listed
};

// Plug-in files in the standard folders (VST 3: Common Files\VST3 and the per-user folder; VST 2: the VSTPluginsPath
// of the registry and the usual install folders), sorted by path.
std::vector<std::pair<std::wstring, PluginFormat>> FindPluginFiles();
// The format of a plug-in file by its name (.vst3 or .dll).
PluginFormat PluginFormatOf(const std::wstring& path);
// Machine type of a PE file (IMAGE_FILE_MACHINE_*), 0 if unreadable; for a VST 3 bundle the x86_64-win binary.
uint16_t PluginMachine(const std::wstring& path);
// The DLL to load for a plug-in path: the binary inside a VST 3 bundle folder, else the path itself.
std::wstring PluginBinaryPath(const std::wstring& path);

// Scan result lines (the scanner's output and the cache file): "class\t..." and "error\t..." lines.
std::wstring FormatScanLines(const PluginFileScan& s);
void ParseScanLine(std::wstring_view line, PluginFileScan& s);

// Scans one file by running `scannerExe --scan-plugin <path>` (killed after timeoutMs).
PluginFileScan ScanPluginFile(const std::wstring& scannerExe, const std::wstring& path, PluginFormat format,
                              uint32_t timeoutMs = 20000);
// Scans every plug-in file, reusing the cached result of unchanged files (%LOCALAPPDATA%\dgmod\plugins.cache) and
// updating the cache. `progress(done, total)` is called from the scanning thread.
std::vector<PluginFileScan> ScanAllPlugins(const std::wstring& scannerExe, bool useCache,
                                           const std::function<void(size_t, size_t)>& progress = {});
// Cached results only (no scanning); empty when nothing was scanned yet.
std::vector<PluginFileScan> LoadPluginCache();

// Text helpers shared by the scanner and the host.
std::wstring FormatClassId(const char bytes[16]);  // 32 upper-case hex digits
bool ParseClassId(std::wstring_view text, char bytes[16]);

}  // namespace dgmod
