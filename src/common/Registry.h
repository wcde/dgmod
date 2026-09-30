#pragma once

#include "common/Win.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dgmod::reg {

struct RawValue {
    DWORD type = REG_NONE;
    std::vector<BYTE> data;
};

Result<UniqueHKey> Open(HKEY root, std::wstring_view path, REGSAM access);
Result<UniqueHKey> Create(HKEY root, std::wstring_view path, REGSAM access, SECURITY_ATTRIBUTES* sa = nullptr);

std::optional<DWORD> ReadDword(HKEY key, const wchar_t* name);
std::optional<std::wstring> ReadString(HKEY key, const wchar_t* name);
std::optional<RawValue> ReadRaw(HKEY key, const wchar_t* name);

HRESULT WriteDword(HKEY key, const wchar_t* name, DWORD value);
HRESULT WriteString(HKEY key, const wchar_t* name, std::wstring_view value, DWORD type = REG_SZ);
HRESULT DeleteValue(HKEY key, const wchar_t* name);  // S_OK when the value did not exist

std::vector<std::wstring> EnumSubkeys(HKEY key);
std::vector<std::wstring> EnumValueNames(HKEY key);

// Convenience one-shot reads.
std::optional<DWORD> ReadDword(HKEY root, std::wstring_view path, const wchar_t* name);
std::optional<std::wstring> ReadString(HKEY root, std::wstring_view path, const wchar_t* name);

}  // namespace dgmod::reg
