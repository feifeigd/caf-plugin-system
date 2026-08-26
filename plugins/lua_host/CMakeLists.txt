# plugins/lua_host/CMakeLists.txt
# Lua 脚本宿主插件：内嵌 Lua（sol2），把 scripts/*.lua 包装成 CAF 服务

find_package(sol2 CONFIG REQUIRED)
# vcpkg lua port 用 CMake 内置 FindLua 模块（usage 明确），非 CONFIG 包
find_package(Lua REQUIRED)

add_library(lua_host_plugin SHARED
    lua_host_plugin.cpp
)

target_include_directories(lua_host_plugin PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(lua_host_plugin PRIVATE caf_plugin_core sol2::sol2 ${LUA_LIBRARIES})

# 定义插件名称（供 logging_service.hpp 的 LOG_* 宏 + PLUGIN_CONFIG 配置段使用）
target_compile_definitions(lua_host_plugin PRIVATE PLUGIN_NAME="lua_host")

set_target_properties(lua_host_plugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/lua_host"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/lua_host"
)

if(WIN32)
    set_target_properties(lua_host_plugin PROPERTIES PREFIX "")
else()
    set_target_properties(lua_host_plugin PROPERTIES PREFIX "lib")
endif()

apply_plugin_rpath(lua_host_plugin)
