cmake_minimum_required(VERSION 3.20)

# ------------------------------------------------------------------
# vcpkg toolchain 必须在 project() 之前设置，否则第一次 configure 不会生效
# ------------------------------------------------------------------
if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

project(caf-plugin-system VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 如果没找到 vcpkg，给出清晰的提示
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE OR NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    message(FATAL_ERROR
        "vcpkg toolchain not found.\n"
        "Please install vcpkg and set VCPKG_ROOT environment variable,\n"
        "or place vcpkg in this project's root directory.\n"
        "See: https://github.com/microsoft/vcpkg#quick-start")
endif()

find_package(CAF REQUIRED COMPONENTS core)

# Core library + Main executable
add_subdirectory(src)

# Plugins
add_subdirectory(plugins)

# Tests
enable_testing()
add_subdirectory(tests)
