cmake_minimum_required(VERSION 3.28)
project(dgmod VERSION 0.1.0 LANGUAGES CXX RC)

if(NOT MSVC)
  message(FATAL_ERROR "dgmod requires MSVC (Windows only)")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Static CRT everywhere: the executables stay self-contained.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(
  UNICODE _UNICODE
  WIN32_LEAN_AND_MEAN NOMINMAX
  _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000010
  DGMOD_VERSION_STR="${PROJECT_VERSION}")

add_compile_options(
  $<$<COMPILE_LANGUAGE:CXX>:/W4>
  $<$<COMPILE_LANGUAGE:CXX>:/permissive->
  $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
  $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>
  $<$<COMPILE_LANGUAGE:CXX>:/Zc:preprocessor>
  $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
  $<$<COMPILE_LANGUAGE:CXX>:/MP>
  $<$<COMPILE_LANGUAGE:CXX>:/Zi>
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Release>>:/O2>
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Release>>:/Gy>
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Release>>:/GL>)

add_link_options(
  /DEBUG
  $<$<CONFIG:Release>:/LTCG>
  $<$<CONFIG:Release>:/OPT:REF>
  $<$<CONFIG:Release>:/OPT:ICF>)

# --- DSP core (portable C++23 + intrinsics, no Windows dependencies) ---
file(GLOB DGMOD_DSP_SOURCES CONFIGURE_DEPENDS src/dsp/*.cpp src/dsp/*.h)
add_library(dgmod_dsp STATIC ${DGMOD_DSP_SOURCES})
target_include_directories(dgmod_dsp PUBLIC ${CMAKE_SOURCE_DIR}/src)

# --- Shared Win32 helpers ---
file(GLOB DGMOD_COMMON_SOURCES CONFIGURE_DEPENDS src/common/*.cpp src/common/*.h)
add_library(dgmod_common STATIC ${DGMOD_COMMON_SOURCES})
target_include_directories(dgmod_common PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(dgmod_common PUBLIC dgmod_dsp advapi32 ole32)

# --- dgmod.exe: settings and monitoring UI ---
file(GLOB_RECURSE DGMOD_APP_SOURCES CONFIGURE_DEPENDS src/app/*.cpp src/app/*.h)
add_executable(dgmod WIN32 ${DGMOD_APP_SOURCES} res/app.rc)
target_include_directories(dgmod PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/app)
target_link_libraries(dgmod PRIVATE dgmod_common dgmod_dsp
  d3d11 dxgi dcomp d2d1 dwrite windowscodecs dwmapi uxtheme
  advapi32 ole32 oleaut32 shell32 shlwapi user32 gdi32 propsys)
# The manifest is embedded through res/app.rc.
target_link_options(dgmod PRIVATE /MANIFEST:NO)
add_dependencies(dgmod dgmod-bridge)

# --- dgmod-bridge.exe: virtual cable -> ASRC oversampler -> DAC in exclusive mode (PCM, DoP) or native DSD (ASIO) ---
add_library(dgmod_bridge_core STATIC src/bridge/Bridge.cpp src/bridge/Bridge.h src/bridge/AsioOutput.cpp
                                     src/bridge/AsioOutput.h src/bridge/SampleWriter.cpp
                                     src/bridge/SampleWriter.h src/bridge/FrameFifo.h src/bridge/DriftController.h
                                     src/bridge/Declicker.h src/bridge/LevelGuard.h src/bridge/SlipCopier.h)
target_link_libraries(dgmod_bridge_core PUBLIC dgmod_common dgmod_dsp dgmod_plugins ole32 avrt propsys user32)

# --- VST 3 / VST 2 hosting (plug-in chain of the bridge, editor windows, scanner) ---
file(GLOB DGMOD_PLUGIN_SOURCES CONFIGURE_DEPENDS src/plugins/*.cpp src/plugins/*.h)
add_library(dgmod_plugins STATIC ${DGMOD_PLUGIN_SOURCES})
target_include_directories(dgmod_plugins PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(dgmod_plugins PUBLIC dgmod_common ole32 user32 dwmapi)

add_executable(dgmod-bridge WIN32 src/bridge/main.cpp src/bridge/PluginTool.cpp src/bridge/PluginTool.h res/bridge.rc)
target_link_libraries(dgmod-bridge PRIVATE dgmod_bridge_core shell32 user32 gdi32 windowscodecs)
# The manifest (per-monitor DPI awareness for plug-in editors) is embedded through res/bridge.rc.
target_link_options(dgmod-bridge PRIVATE /MANIFEST:NO)
