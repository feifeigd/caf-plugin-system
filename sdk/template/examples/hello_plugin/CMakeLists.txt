# hello_plugin — 独立插件工程示例（消费 caf-plugin-sdk）
#
# 前置：
#   1. vcpkg 已安装并设置 VCPKG_ROOT（或 -DCMAKE_TOOLCHAIN_FILE=...）
#   2. 本示例目录已拷出 SDK 包（vcpkg.json 与本 CMakeLists 同级），
#      首次 configure 时 vcpkg 会自动安装 caf 1.1.0 / spdlog / fmt
#   3. -DCAF_PLUGIN_SDK_ROOT 指向 caf-plugin-sdk 目录
#
# 构建：
#   cmake -B out -G "Visual Studio 17 2022" -A x64 ^
#         -DCAF_PLUGIN_SDK_ROOT=G:\path\to\caf-plugin-sdk
#   cmake --build out --config Debug
#
# 产物 out/Debug/hello_plugin.dll → 部署到 run/<Config>/plugins/hello/

cmake_minimum_required(VERSION 3.20)
project(hello_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# UTF-8 源码（中文注释/字符串）在 MSVC 默认 936 代码页下解析会错乱
if(MSVC)
    add_compile_options(/utf-8)
endif()

# vcpkg 工具链（示例自带 vcpkg.json 时由工具链自动装依赖；此处给出候选路径）
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    foreach(candidate
            "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg/scripts/buildsystems/vcpkg.cmake"
            "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake")
        if(EXISTS "${candidate}")
            set(CMAKE_TOOLCHAIN_FILE "${candidate}" CACHE STRING "")
            break()
        endif()
    endforeach()
endif()

if(NOT CAF_PLUGIN_SDK_ROOT)
    message(FATAL_ERROR
        "请用 -DCAF_PLUGIN_SDK_ROOT=<caf-plugin-sdk 目录> 指定 SDK 路径")
endif()

list(APPEND CMAKE_PREFIX_PATH "${CAF_PLUGIN_SDK_ROOT}")
find_package(caf-plugin-sdk CONFIG REQUIRED)

add_library(hello_plugin SHARED hello_plugin.cpp)
target_link_libraries(hello_plugin PRIVATE caf::plugin_core)

# 插件名：logging_service.hpp 的 LOG_* 宏用它做日志来源名
target_compile_definitions(hello_plugin PRIVATE PLUGIN_NAME="hello")

if(WIN32)
    set_target_properties(hello_plugin PROPERTIES PREFIX "")
else()
    set_target_properties(hello_plugin PROPERTIES PREFIX "lib")
endif()
