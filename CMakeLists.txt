cmake_minimum_required(VERSION 3.20)

# ------------------------------------------------------------------
# vcpkg toolchain 必须在 project() 之前设置
# 优先级：1) 环境变量 2) 项目同级目录 3) VS2022 自带
# ------------------------------------------------------------------
set(VCPKG_TOOLCHAIN_CANDIDATES
    "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg/scripts/buildsystems/vcpkg.cmake"
    "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/vcpkg/scripts/buildsystems/vcpkg.cmake"
)

foreach(candidate IN LISTS VCPKG_TOOLCHAIN_CANDIDATES)
    if(EXISTS "${candidate}")
        set(CMAKE_TOOLCHAIN_FILE "${candidate}" CACHE STRING "")
        message(STATUS "vcpkg toolchain: ${candidate}")
        break()
    endif()
endforeach()

project(caf-plugin-system VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# UTF-8 源码（中文注释/字符串）在 MSVC 默认 936 代码页下解析会错乱
# （C4819 + 后续 C2065 误报）。/utf-8 让 MSVC 按 UTF-8 解析源码。
if(MSVC)
    add_compile_options(/utf-8)
endif()

# 如果没找到 vcpkg，给出清晰的提示
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE OR NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    message(FATAL_ERROR
        "vcpkg toolchain not found.\n"
        "Please install vcpkg and set VCPKG_ROOT environment variable,\n"
        "or place vcpkg in this project's root directory.\n"
        "See: https://github.com/microsoft/vcpkg#quick-start")
endif()

find_package(CAF REQUIRED COMPONENTS core)
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)

# 启用 CAF 框架日志（所有级别）
add_compile_definitions(CAF_LOG_LEVEL=CAF_LOG_LEVEL_DEBUG)
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)

# Core library + Main executable
add_subdirectory(src)

# Plugins
add_subdirectory(plugins)

# Tests
enable_testing()
add_subdirectory(tests)
