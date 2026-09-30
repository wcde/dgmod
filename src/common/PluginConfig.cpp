#include "common/PluginConfig.h"

#include "common/BridgeConfig.h"
#include "common/Registry.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

namespace dgmod {

namespace {

namespace fs = std::filesystem;

std::wstring Clean(std::wstring s) {
    for (auto& c : s)
        if (c == L'\t' || c == L'\n' || c == L'\r') c = L' ';
    return s;
}

std::vector<std::wstring_view> Split(std::wstring_view s, wchar_t sep) {
    std::vector<std::wstring_view> out;
    for (size_t start = 0;;) {
        const size_t end = s.find(sep, start);
        out.push_back(s.substr(start, end == std::wstring_view::npos ? std::wstring_view::npos : end - start));
        if (end == std::wstring_view::npos) break;
        start = end + 1;
    }
    return out;
}

uint64_t ToU64(std::wstring_view s) {
    uint64_t v = 0;
    for (const wchar_t c : s) {
        if (c < L'0' || c > L'9') break;
        v = v * 10 + uint64_t(c - L'0');
    }
    return v;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool EndsWithNoCase(const std::wstring& s, std::wstring_view suffix) {
    return s.size() >= suffix.size() && Lower(s.substr(s.size() - suffix.size())) == suffix;
}

std::wstring Env(const wchar_t* name) {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = ::GetEnvironmentVariableW(name, buf, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

void FileStamp(const std::wstring& path, uint64_t& writeTime, uint64_t& size) {
    writeTime = size = 0;
    const std::wstring bin = PluginBinaryPath(path);
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!::GetFileAttributesExW(bin.c_str(), GetFileExInfoStandard, &a)) return;
    writeTime = (uint64_t{a.ftLastWriteTime.dwHighDateTime} << 32) | a.ftLastWriteTime.dwLowDateTime;
    size = (uint64_t{a.nFileSizeHigh} << 32) | a.nFileSizeLow;
}

std::wstring CachePath() {
    const std::wstring dir = DataDirectory();
    return dir.empty() ? std::wstring() : dir + L"\\plugins.cache";
}

void SaveCache(const std::vector<PluginFileScan>& scans) {
    const std::wstring path = CachePath();
    if (path.empty()) return;
    std::wstring text;
    for (const auto& s : scans) {
        text += std::format(L"file\t{}\t{}\t{}\t{}\n", static_cast<uint32_t>(s.format), s.writeTime, s.size, s.path);
        text += FormatScanLines(s);
    }
    const std::string utf8 = Narrow(text);
    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    }
    ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

}  // namespace

// ---- Chain -----------------------------------------------------------------------------------------------------------

uint32_t PluginChainConfig::NextId() const {
    uint32_t id = 1;
    for (const auto& p : plugins) id = std::max(id, p.id + 1);
    return id;
}

const PluginEntry* PluginChainConfig::Find(uint32_t id) const {
    for (const auto& p : plugins)
        if (p.id == id) return &p;
    return nullptr;
}

PluginChainConfig LoadPluginChain(const wchar_t* keyPath) {
    PluginChainConfig c;
    auto key = reg::Open(HKEY_CURRENT_USER, keyPath, KEY_QUERY_VALUE);
    if (!key) return c;
    if (auto v = reg::ReadDword(key->Get(), L"Enabled")) c.enabled = *v != 0;
    const std::wstring chain = reg::ReadString(key->Get(), L"Chain").value_or(L"");
    for (const auto line : Split(chain, L'\n')) {
        const auto f = Split(line, L'\t');
        if (f.size() < 8) continue;
        PluginEntry e;
        e.id = static_cast<uint32_t>(ToU64(f[0]));
        e.format = ToU64(f[1]) == 1 ? PluginFormat::Vst2 : PluginFormat::Vst3;
        e.enabled = f[2] != L"0";
        e.quarantined = f[3] == L"1";
        e.path = f[4];
        e.classId = f[5];
        e.name = f[6];
        e.vendor = f[7];
        e.windowsScaling = f.size() > 8 && f[8] == L"1";
        if (!e.id || e.path.empty() || c.Find(e.id) || c.plugins.size() == kMaxPlugins) continue;
        c.plugins.push_back(std::move(e));
    }
    return c;
}

HRESULT SavePluginChain(const PluginChainConfig& c, const wchar_t* keyPath) {
    auto key = reg::Create(HKEY_CURRENT_USER, keyPath, KEY_SET_VALUE);
    if (!key) return key.error().hr;
    std::wstring chain;
    for (const auto& e : c.plugins) {
        if (!chain.empty()) chain += L'\n';
        chain += std::format(L"{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", e.id, static_cast<uint32_t>(e.format), e.enabled ? 1 : 0,
                             e.quarantined ? 1 : 0, Clean(e.path), Clean(e.classId), Clean(e.name), Clean(e.vendor),
                             e.windowsScaling ? 1 : 0);
    }
    HRESULT hr = reg::WriteDword(key->Get(), L"Enabled", c.enabled ? 1u : 0u);
    if (SUCCEEDED(hr)) hr = reg::WriteString(key->Get(), L"Chain", chain);
    return hr;
}

std::wstring PluginDataDirectory() {
    const std::wstring base = DataDirectory();
    if (base.empty()) return {};
    const std::wstring dir = base + L"\\plugins";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring PluginStatePath(uint32_t id) {
    const std::wstring dir = PluginDataDirectory();
    return dir.empty() ? std::wstring() : std::format(L"{}\\{}.state", dir, id);
}

// ---- Files -----------------------------------------------------------------------------------------------------------

PluginFormat PluginFormatOf(const std::wstring& path) {
    return EndsWithNoCase(path, L".vst3") ? PluginFormat::Vst3 : PluginFormat::Vst2;
}

std::wstring PluginBinaryPath(const std::wstring& path) {
    std::error_code ec;
    if (!fs::is_directory(path, ec)) return path;
    const fs::path bundle(path);
    const fs::path dir = bundle / L"Contents" / L"x86_64-win";
    const fs::path named = dir / bundle.filename();
    if (fs::is_regular_file(named, ec)) return named.wstring();
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && EndsWithNoCase(e.path().wstring(), L".vst3")) return e.path().wstring();
    return named.wstring();
}

uint16_t PluginMachine(const std::wstring& path) {
    UniqueHandle f(::CreateFileW(PluginBinaryPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, 0, nullptr));
    if (!f) return 0;
    auto read = [&](LONG offset, void* dst, DWORD size) {
        DWORD got = 0;
        return ::SetFilePointer(f.Get(), offset, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
               ::ReadFile(f.Get(), dst, size, &got, nullptr) && got == size;
    };
    IMAGE_DOS_HEADER dos{};
    if (!read(0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
    DWORD sig = 0;
    IMAGE_FILE_HEADER fh{};
    if (!read(dos.e_lfanew, &sig, sizeof(sig)) || sig != IMAGE_NT_SIGNATURE) return 0;
    if (!read(dos.e_lfanew + 4, &fh, sizeof(fh))) return 0;
    return fh.Machine;
}

std::vector<std::pair<std::wstring, PluginFormat>> FindPluginFiles() {
    std::vector<std::pair<std::wstring, PluginFormat>> out;
    std::map<std::wstring, bool> seen;
    auto add = [&](const std::wstring& p, PluginFormat f) {
        if (seen.emplace(Lower(p), true).second) out.emplace_back(p, f);
    };
    std::error_code ec;
    auto walk = [&](const std::wstring& root, PluginFormat format) {
        if (root.empty() || !fs::is_directory(root, ec)) return;
        auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
        for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            const fs::directory_entry& e = *it;
            const std::wstring p = e.path().wstring();
            if (format == PluginFormat::Vst3) {
                if (!EndsWithNoCase(p, L".vst3")) continue;
                add(p, format);
                if (e.is_directory(ec)) it.disable_recursion_pending();
            } else if (e.is_regular_file(ec) && EndsWithNoCase(p, L".dll")) {
                add(p, format);
            }
            if (it.depth() > 6) it.disable_recursion_pending();
        }
        ec.clear();
    };
    const std::wstring common = Env(L"CommonProgramFiles"), programs = Env(L"ProgramFiles"), local = Env(L"LOCALAPPDATA");
    walk(common + L"\\VST3", PluginFormat::Vst3);
    if (!local.empty()) walk(local + L"\\Programs\\Common\\VST3", PluginFormat::Vst3);
    std::vector<std::wstring> vst2;
    for (const HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER})
        if (auto v = reg::ReadString(root, L"SOFTWARE\\VST", L"VSTPluginsPath"); v && !v->empty()) vst2.push_back(*v);
    for (const wchar_t* sub : {L"\\VSTPlugins", L"\\Steinberg\\VSTPlugins"}) vst2.push_back(programs + sub);
    for (const wchar_t* sub : {L"\\VST2", L"\\Steinberg\\VST2", L"\\VST"}) vst2.push_back(common + sub);
    for (const auto& d : vst2) walk(d, PluginFormat::Vst2);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return Lower(a.first) < Lower(b.first); });
    return out;
}

// ---- Scanning --------------------------------------------------------------------------------------------------------

std::wstring FormatScanLines(const PluginFileScan& s) {
    std::wstring out;
    if (s.notPlugin) out += L"notplugin\n";
    if (!s.error.empty()) out += L"error\t" + Clean(s.error) + L"\n";
    for (const auto& c : s.classes)
        out += std::format(L"class\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", static_cast<uint32_t>(c.format), Clean(c.classId),
                           c.instrument ? 1 : 0, Clean(c.name), Clean(c.vendor), Clean(c.category), Clean(c.version));
    return out;
}

void ParseScanLine(std::wstring_view line, PluginFileScan& s) {
    while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n')) line.remove_suffix(1);
    const auto f = Split(line, L'\t');
    if (f[0] == L"notplugin") {
        s.notPlugin = true;
    } else if (f[0] == L"error" && f.size() >= 2) {
        s.error = f[1];
    } else if (f[0] == L"class" && f.size() >= 8) {
        PluginClassInfo c;
        c.format = ToU64(f[1]) == 1 ? PluginFormat::Vst2 : PluginFormat::Vst3;
        c.path = s.path;
        c.classId = f[2];
        c.instrument = f[3] == L"1";
        c.name = f[4];
        c.vendor = f[5];
        c.category = f[6];
        c.version = f[7];
        s.classes.push_back(std::move(c));
    }
}

PluginFileScan ScanPluginFile(const std::wstring& scannerExe, const std::wstring& path, PluginFormat format,
                              uint32_t timeoutMs) {
    PluginFileScan s;
    s.path = path;
    s.format = format;
    FileStamp(path, s.writeTime, s.size);
    const uint16_t machine = PluginMachine(path);
    if (machine == IMAGE_FILE_MACHINE_I386) {
        s.error = L"32-bit plug-in: dgmod loads 64-bit plug-ins only";
        return s;
    }
    if (machine != IMAGE_FILE_MACHINE_AMD64) {
        if (format == PluginFormat::Vst2) s.notPlugin = true;
        else s.error = L"No 64-bit Windows binary";
        return s;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!::CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        s.error = L"Cannot create a pipe for the scanner";
        return s;
    }
    UniqueHandle rd(readPipe), wr(writePipe);
    ::SetHandleInformation(rd.Get(), HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr.Get();
    si.hStdError = wr.Get();
    si.hStdInput = nullptr;
    std::wstring cmd = std::format(L"\"{}\" --scan-plugin \"{}\"", scannerExe, path);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(scannerExe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        s.error = L"Cannot start the scanner: " + HResultText(HRESULT_FROM_WIN32(::GetLastError()));
        return s;
    }
    UniqueHandle process(pi.hProcess), thread(pi.hThread);
    wr.Reset();  // the child holds the only write end: ReadFile ends when it exits
    std::string output;
    std::thread reader([&] {
        char buf[4096];
        DWORD got = 0;
        while (::ReadFile(rd.Get(), buf, sizeof(buf), &got, nullptr) && got) output.append(buf, got);
    });
    const DWORD w = ::WaitForSingleObject(process.Get(), timeoutMs);
    if (w != WAIT_OBJECT_0) ::TerminateProcess(process.Get(), 1);
    reader.join();
    DWORD code = 0;
    ::GetExitCodeProcess(process.Get(), &code);
    const std::wstring text = Widen(output);
    for (const auto line : Split(text, L'\n'))
        if (!line.empty()) ParseScanLine(line, s);
    if (w != WAIT_OBJECT_0) {
        s.classes.clear();
        s.error = L"The plug-in did not finish loading within " + std::to_wstring(timeoutMs / 1000) + L" s";
    } else if (s.classes.empty() && s.error.empty() && !s.notPlugin) {
        s.error = std::format(L"The plug-in crashed while loading (exit code 0x{:08X})", code);
    }
    return s;
}

std::vector<PluginFileScan> LoadPluginCache() {
    std::vector<PluginFileScan> out;
    const std::wstring path = CachePath();
    if (path.empty()) return out;
    std::ifstream f(path, std::ios::binary);
    if (!f) return out;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::wstring text = Widen(ss.str());
    for (const auto line : Split(text, L'\n')) {
        if (line.empty()) continue;
        if (line.starts_with(L"file\t")) {
            const auto p = Split(line, L'\t');
            if (p.size() < 5) continue;
            PluginFileScan s;
            s.format = ToU64(p[1]) == 1 ? PluginFormat::Vst2 : PluginFormat::Vst3;
            s.writeTime = ToU64(p[2]);
            s.size = ToU64(p[3]);
            s.path = p[4];
            out.push_back(std::move(s));
        } else if (!out.empty()) {
            ParseScanLine(line, out.back());
        }
    }
    return out;
}

std::vector<PluginFileScan> ScanAllPlugins(const std::wstring& scannerExe, bool useCache,
                                           const std::function<void(size_t, size_t)>& progress) {
    const auto files = FindPluginFiles();
    std::map<std::wstring, PluginFileScan> cache;
    if (useCache)
        for (auto& s : LoadPluginCache()) cache.emplace(Lower(s.path), std::move(s));
    std::vector<PluginFileScan> out(files.size());
    std::vector<size_t> todo;
    for (size_t i = 0; i < files.size(); ++i) {
        const auto it = cache.find(Lower(files[i].first));
        uint64_t t = 0, size = 0;
        FileStamp(files[i].first, t, size);
        if (it != cache.end() && it->second.writeTime == t && it->second.size == size && t) out[i] = it->second;
        else todo.push_back(i);
    }
    std::atomic<size_t> next{0}, done{files.size() - todo.size()};
    if (progress) progress(done.load(), files.size());
    std::mutex progressMutex;
    const unsigned workers = std::clamp<unsigned>(std::thread::hardware_concurrency() / 2, 1, 4);
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < workers; ++w)
        pool.emplace_back([&] {
            for (;;) {
                const size_t k = next.fetch_add(1);
                if (k >= todo.size()) return;
                const size_t i = todo[k];
                out[i] = ScanPluginFile(scannerExe, files[i].first, files[i].second);
                const size_t d = done.fetch_add(1) + 1;
                if (progress) {
                    std::lock_guard lock(progressMutex);
                    progress(d, files.size());
                }
            }
        });
    for (auto& t : pool) t.join();
    SaveCache(out);
    return out;
}

// ---- Class IDs -------------------------------------------------------------------------------------------------------

std::wstring FormatClassId(const char bytes[16]) {
    std::wstring s;
    for (int i = 0; i < 16; ++i) s += std::format(L"{:02X}", static_cast<uint8_t>(bytes[i]));
    return s;
}

bool ParseClassId(std::wstring_view text, char bytes[16]) {
    if (text.size() != 32) return false;
    auto hex = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        return -1;
    };
    for (int i = 0; i < 16; ++i) {
        const int hi = hex(text[size_t(i) * 2]), lo = hex(text[size_t(i) * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        bytes[i] = static_cast<char>(hi * 16 + lo);
    }
    return true;
}

}  // namespace dgmod
