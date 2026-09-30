#pragma once

// Plug-in commands of dgmod-bridge.exe (--scan-plugin, --plugin-test); see PluginTool.cpp.

#include <functional>
#include <string>
#include <vector>

namespace dgmod::bridge {

int ScanPluginCommand(const std::wstring& path, const std::function<void(const std::wstring&)>& print);
int PluginTestCommand(const std::vector<std::wstring>& args, const std::function<void(const std::wstring&)>& print);
int PluginHostTestCommand(const std::vector<std::wstring>& args, const std::function<void(const std::wstring&)>& print);

}  // namespace dgmod::bridge
