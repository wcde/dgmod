#pragma once

// Installed ASIO drivers (HKLM\SOFTWARE\ASIO, the view of the current process: 64-bit drivers for x64 builds).

#include "common/Win.h"

#include <string>
#include <string_view>
#include <vector>

namespace dgmod {

struct AsioDriverEntry {
    std::wstring name;  // registry key name; what hosts show and what the bridge configuration stores
    CLSID clsid{};
};

std::vector<AsioDriverEntry> ListAsioDrivers();

// Driver for the configured name; an empty name picks the driver that matches the DAC's endpoint name (for example
// "FiiO ASIO Driver" for "Speakers (2- FiiO K9)"). Returns nullptr if there is none.
const AsioDriverEntry* FindAsioDriver(const std::vector<AsioDriverEntry>& drivers, std::wstring_view configured,
                                      std::wstring_view endpointName);

}  // namespace dgmod
