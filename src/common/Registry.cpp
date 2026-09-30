#include "common/Registry.h"

namespace dgmod::reg {

namespace {

std::wstring Z(std::wstring_view s) { return std::wstring(s); }

}  // namespace

Result<UniqueHKey> Open(HKEY root, std::wstring_view path, REGSAM access) {
    UniqueHKey key;
    const LSTATUS st = ::RegOpenKeyExW(root, Z(path).c_str(), 0, access, key.Put());
    if (st != ERROR_SUCCESS) return FailWin32(L"RegOpenKeyEx " + Z(path), static_cast<DWORD>(st));
    return key;
}

Result<UniqueHKey> Create(HKEY root, std::wstring_view path, REGSAM access, SECURITY_ATTRIBUTES* sa) {
    UniqueHKey key;
    const LSTATUS st = ::RegCreateKeyExW(root, Z(path).c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, access, sa,
                                         key.Put(), nullptr);
    if (st != ERROR_SUCCESS) return FailWin32(L"RegCreateKeyEx " + Z(path), static_cast<DWORD>(st));
    return key;
}

std::optional<RawValue> ReadRaw(HKEY key, const wchar_t* name) {
    DWORD type = 0, size = 0;
    if (::RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) return std::nullopt;
    RawValue v;
    v.type = type;
    v.data.resize(size);
    if (::RegQueryValueExW(key, name, nullptr, &type, v.data.data(), &size) != ERROR_SUCCESS) return std::nullopt;
    v.data.resize(size);
    return v;
}

std::optional<DWORD> ReadDword(HKEY key, const wchar_t* name) {
    DWORD value = 0, size = sizeof(value), type = 0;
    if (::RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) != ERROR_SUCCESS ||
        type != REG_DWORD)
        return std::nullopt;
    return value;
}

std::optional<std::wstring> ReadString(HKEY key, const wchar_t* name) {
    auto raw = ReadRaw(key, name);
    if (!raw || (raw->type != REG_SZ && raw->type != REG_EXPAND_SZ)) return std::nullopt;
    std::wstring s(reinterpret_cast<const wchar_t*>(raw->data.data()), raw->data.size() / sizeof(wchar_t));
    while (!s.empty() && s.back() == L'\0') s.pop_back();
    return s;
}

HRESULT WriteDword(HKEY key, const wchar_t* name, DWORD value) {
    const LSTATUS st = ::RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    return HRESULT_FROM_WIN32(st);
}

HRESULT WriteString(HKEY key, const wchar_t* name, std::wstring_view value, DWORD type) {
    const std::wstring s(value);
    const LSTATUS st = ::RegSetValueExW(key, name, 0, type, reinterpret_cast<const BYTE*>(s.c_str()),
                                        static_cast<DWORD>((s.size() + 1) * sizeof(wchar_t)));
    return HRESULT_FROM_WIN32(st);
}

HRESULT DeleteValue(HKEY key, const wchar_t* name) {
    const LSTATUS st = ::RegDeleteValueW(key, name);
    return st == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(st);
}

std::vector<std::wstring> EnumSubkeys(HKEY key) {
    std::vector<std::wstring> out;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = static_cast<DWORD>(std::size(name));
        const LSTATUS st = ::RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (st != ERROR_SUCCESS) break;
        out.emplace_back(name, len);
    }
    return out;
}

std::vector<std::wstring> EnumValueNames(HKEY key) {
    std::vector<std::wstring> out;
    wchar_t name[512];
    for (DWORD i = 0;; ++i) {
        DWORD len = static_cast<DWORD>(std::size(name));
        const LSTATUS st = ::RegEnumValueW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (st != ERROR_SUCCESS) break;
        out.emplace_back(name, len);
    }
    return out;
}

std::optional<DWORD> ReadDword(HKEY root, std::wstring_view path, const wchar_t* name) {
    auto key = Open(root, path, KEY_QUERY_VALUE);
    return key ? ReadDword(key->Get(), name) : std::nullopt;
}

std::optional<std::wstring> ReadString(HKEY root, std::wstring_view path, const wchar_t* name) {
    auto key = Open(root, path, KEY_QUERY_VALUE);
    return key ? ReadString(key->Get(), name) : std::nullopt;
}

}  // namespace dgmod::reg
