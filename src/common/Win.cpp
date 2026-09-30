#include "common/Win.h"

#include <format>

namespace dgmod {

std::wstring Error::Message() const {
    return what.empty() ? HResultText(hr) : std::format(L"{}: {}", what, HResultText(hr));
}

std::wstring HResultText(HRESULT hr) {
    wchar_t* buf = nullptr;
    const DWORD n = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                         FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, static_cast<DWORD>(hr), MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                                     reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring text;
    if (n && buf) {
        text.assign(buf, n);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' ||
                                 text.back() == L'.'))
            text.pop_back();
    }
    if (buf) ::LocalFree(buf);
    const auto code = std::format(L"0x{:08X}", static_cast<uint32_t>(hr));
    return text.empty() ? code : std::format(L"{} ({})", text, code);
}

std::wstring Widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string Narrow(std::wstring_view s) {
    if (s.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace dgmod
