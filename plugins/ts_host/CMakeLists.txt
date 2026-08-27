# plugins/ts_host/CMakeLists.txt
# TypeScript 脚本宿主插件：内嵌 QuickJS（vendor 源码），构建时预编译 .ts → .js。

# QuickJS 是 C 源文件，需启用 C 语言（根 project 只开了 CXX）
enable_language(C)

add_library(ts_host_plugin SHARED
    ts_host_plugin.cpp
    quickjs/quickjs.c
    quickjs/libregexp.c
    quickjs/libunicode.c
    quickjs/dtoa.c
)

target_include_directories(ts_host_plugin PRIVATE quickjs)

target_link_libraries(ts_host_plugin PRIVATE caf_plugin_core)

# QuickJS 用 strcpy/snprintf 等，MSVC 会报警告；C 原子需要 C11；
# WIN32_LEAN_AND_MEAN 避免 windows.h 引入 winsock.h 与 quickjs-libc 的 winsock2.h 冲突
target_compile_definitions(ts_host_plugin PRIVATE
    PLUGIN_NAME="ts_host"
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
)

set_target_properties(ts_host_plugin PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)

set_target_properties(ts_host_plugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/ts_host"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/ts_host"
)

if(WIN32)
    set_target_properties(ts_host_plugin PROPERTIES PREFIX "")
else()
    set_target_properties(ts_host_plugin PROPERTIES PREFIX "lib")
endif()

apply_plugin_rpath(ts_host_plugin)
