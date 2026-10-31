# dgmod-receiver: Linux (e.g. Raspberry Pi OS). Build on the Pi:
#   sudo apt install build-essential cmake libasound2-dev
#   cmake -S receiver -B build-receiver -DCMAKE_BUILD_TYPE=Release && cmake --build build-receiver
cmake_minimum_required(VERSION 3.16)
project(dgmod-receiver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(ALSA REQUIRED)
find_package(Threads REQUIRED)

add_executable(dgmod-receiver main.cpp Receiver.cpp Receiver.h AlsaSink.cpp AlsaSink.h)
# The wire protocol is shared with the bridge.
target_include_directories(dgmod-receiver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(dgmod-receiver PRIVATE -Wall -Wextra)
target_link_libraries(dgmod-receiver PRIVATE ALSA::ALSA Threads::Threads)

install(TARGETS dgmod-receiver RUNTIME DESTINATION bin)
