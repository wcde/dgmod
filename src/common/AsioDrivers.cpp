#include "common/AsioDrivers.h"

#include "common/BridgeConfig.h"
#include "common/Registry.h"

#include <algorithm>
#include <cwctype>

namespace dgmod {

namespace {

constexpr wchar_t kAsioKey[] = L"SOFTWARE\\ASIO";

std::wstring Lower(std::wstring_view s) {
    std::wstring out(s);
    for (auto& ch : out) ch = static_cast<wchar_t>(std::towlower(ch));
    return out;
}

// Distinctive words of a name ("Speakers (FiiO K9)" -> "fiio", "k9"); generic audio words never identify a vendor.
std::vector<std::wstring> Words(std::wstring_view name) {
    static constexpr std::wstring_view kGeneric[] = {L"usb",     L"audio",    L"asio",   L"driver", L"speakers", L"speaker",
                                                     L"headphones", L"headphone", L"device", L"digital", L"output",
                                                     L"line",    L"high",     L"definition", L"interface", L"dac",
                                                     L"sound",   L"the",      L"and",    L"amp",    L"out",    L"2ch"};
    std::vector<std::wstring> words;
    std::wstring cur;
    auto flush = [&] {
        if (cur.size() >= 2 && std::find(std::begin(kGeneric), std::end(kGeneric), cur) == std::end(kGeneric))
            words.push_back(cur);
        cur.clear();
    };
    for (const wchar_t ch : Lower(name)) {
        if (std::iswalnum(ch)) cur += ch;
        else flush();
    }
    flush();
    return words;
}

}  // namespace

std::vector<AsioDriverEntry> ListAsioDrivers() {
    std::vector<AsioDriverEntry> out;
    auto root = reg::Open(HKEY_LOCAL_MACHINE, kAsioKey, KEY_READ);
    if (!root) return out;
    for (const auto& name : reg::EnumSubkeys(root->Get())) {
        const auto clsid = reg::ReadString(HKEY_LOCAL_MACHINE, std::wstring(kAsioKey) + L"\\" + name, L"CLSID");
        AsioDriverEntry e{name, {}};
        if (!clsid || FAILED(::CLSIDFromString(clsid->c_str(), &e.clsid))) continue;
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const AsioDriverEntry& a, const AsioDriverEntry& b) { return Lower(a.name) < Lower(b.name); });
    return out;
}

const AsioDriverEntry* FindAsioDriver(const std::vector<AsioDriverEntry>& drivers, std::wstring_view configured,
                                      std::wstring_view endpointName) {
    if (!configured.empty()) {
        for (const auto& d : drivers)
            if (Lower(d.name) == Lower(configured)) return &d;
        return nullptr;
    }
    const auto wanted = Words(NormalizeEndpointName(endpointName));
    const AsioDriverEntry* best = nullptr;
    size_t bestScore = 0;
    for (const auto& d : drivers) {
        const auto have = Words(d.name);
        const auto score = static_cast<size_t>(std::count_if(wanted.begin(), wanted.end(), [&](const std::wstring& w) {
            return std::find(have.begin(), have.end(), w) != have.end();
        }));
        if (score > bestScore) {
            best = &d;
            bestScore = score;
        }
    }
    return best;
}

}  // namespace dgmod
