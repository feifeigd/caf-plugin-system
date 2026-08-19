cmake_minimum_required(VERSION 3.20)
project(caf-plugin-system VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# vcpkg integration (manifest mode)
if(DEFINED ENV{VCPKG_ROOT})
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

find_package(CAF REQUIRED COMPONENTS core)

# Core library + Main executable
add_subdirectory(src)

# Plugins
add_subdirectory(plugins)

# Tests
enable_testing()
add_subdirectory(tests)
