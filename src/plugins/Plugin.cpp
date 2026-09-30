#include "plugins/Plugin.h"

#include <malloc.h>

#include <algorithm>
#include <format>

namespace dgmod::plugins {

// No C++ objects with destructors in this frame: __try cannot be mixed with them.
uint32_t GuardedCall(void (*fn)(void*), void* ctx) {
    __try {
        fn(ctx);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        const uint32_t code = GetExceptionCode();
        if (code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
        return code ? code : 1u;
    }
}

std::wstring ExceptionName(uint32_t code) {
    const wchar_t* name = nullptr;
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: name = L"access violation"; break;
        case EXCEPTION_STACK_OVERFLOW: name = L"stack overflow"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION: name = L"illegal instruction"; break;
        case EXCEPTION_PRIV_INSTRUCTION: name = L"privileged instruction"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO: name = L"integer division by zero"; break;
        case EXCEPTION_DATATYPE_MISALIGNMENT: name = L"misaligned access"; break;
        case EXCEPTION_IN_PAGE_ERROR: name = L"page error"; break;
        case 0xE06D7363: name = L"C++ exception"; break;
        default: break;
    }
    return name ? std::format(L"{} (0x{:08X})", name, code) : std::format(L"exception 0x{:08X}", code);
}

void MapInput(const float* const* in, uint32_t channels, float* const* bus, uint32_t busChannels, uint32_t frames) {
    if (busChannels == 1 && channels >= 2) {
        for (uint32_t i = 0; i < frames; ++i) bus[0][i] = 0.5f * (in[0][i] + in[1][i]);
        return;
    }
    for (uint32_t c = 0; c < busChannels; ++c) {
        if (c < channels) std::copy_n(in[c], frames, bus[c]);
        else if (channels == 1) std::copy_n(in[0], frames, bus[c]);  // a mono stream feeds every input
        else std::fill_n(bus[c], frames, 0.0f);
    }
}

void MapOutput(const float* const* busOut, uint32_t busChannels, const float* const* in, float* const* out, uint32_t channels,
               uint32_t frames) {
    for (uint32_t c = 0; c < channels; ++c) {
        if (busChannels == 1) std::copy_n(busOut[0], frames, out[c]);
        else if (c < busChannels) std::copy_n(busOut[c], frames, out[c]);
        else std::copy_n(in[c], frames, out[c]);
    }
}

HMODULE LoadPluginModule(const std::wstring& binary, std::wstring& error) {
    HMODULE m = nullptr;
    const uint32_t code = Guarded([&] {
        m = ::LoadLibraryExW(binary.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    });
    if (code) {
        error = L"The plug-in crashed while loading: " + ExceptionName(code);
        return nullptr;
    }
    if (!m) {
        const DWORD err = ::GetLastError();
        error = err == ERROR_BAD_EXE_FORMAT ? std::wstring(L"Not a 64-bit plug-in")
                                            : L"Cannot load the plug-in: " + HResultText(HRESULT_FROM_WIN32(err));
        if (err == ERROR_MOD_NOT_FOUND)
            if (const std::wstring missing = MissingDependencies(binary); !missing.empty())
                error = L"The plug-in needs " + missing + L", not installed (reinstall the plug-in)";
    }
    return m;
}

std::wstring MissingDependencies(const std::wstring& binary) {
    // Map the image without resolving its imports, then look for every DLL it imports the way the loader would (the
    // plug-in's folder first).
    HMODULE image = ::LoadLibraryExW(binary.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!image) return {};
    std::wstring missing;
    const auto* base = reinterpret_cast<const uint8_t*>(image);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const std::wstring folder = binary.substr(0, binary.find_last_of(L"\\/") + 1);
    if (dir.VirtualAddress && dir.Size) {
        for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
            const std::wstring name = Widen(reinterpret_cast<const char*>(base + d->Name));
            if (::GetFileAttributesW((folder + name).c_str()) != INVALID_FILE_ATTRIBUTES) continue;
            if (HMODULE dep = ::LoadLibraryExW(name.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE)) {
                ::FreeLibrary(dep);
                continue;
            }
            missing += (missing.empty() ? L"" : L", ") + name;
        }
    }
    ::FreeLibrary(image);
    return missing;
}

Result<std::unique_ptr<PluginInstance>> LoadPlugin(const PluginEntry& e, PluginCallbacks* cb) {
    return e.format == PluginFormat::Vst3 ? LoadVst3(e.path, e.classId, cb) : LoadVst2(e.path, e.classId, cb);
}

PluginFileScan ScanPluginInProcess(const std::wstring& path) {
    PluginFileScan s;
    s.path = path;
    s.format = PluginFormatOf(path);
    if (s.format == PluginFormat::Vst3) ScanVst3(path, s);
    else ScanVst2(path, s);
    return s;
}

}  // namespace dgmod::plugins
