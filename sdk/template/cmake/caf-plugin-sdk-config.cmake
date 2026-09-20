# caf-plugin-sdk CMake 包配置
#
# 用法：
#   set(CMAKE_PREFIX_PATH "<sdk-root>" ${CMAKE_PREFIX_PATH})
#   find_package(caf-plugin-sdk CONFIG REQUIRED)
#   target_link_libraries(my_plugin PRIVATE caf::plugin_core)
#
# 前提：vcpkg 工具链已配置，且已安装 caf 1.1.0 / fmt / spdlog
#（见 SDK 根目录 vcpkg.json，版本必须与本 SDK 对应的主程序一致）。

get_filename_component(CAF_PLUGIN_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

include(CMakeFindDependencyMacro)
find_dependency(CAF COMPONENTS core io)
find_dependency(fmt CONFIG)
find_dependency(spdlog CONFIG)

add_library(caf_plugin_core SHARED IMPORTED)

set_target_properties(caf_plugin_core PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${CAF_PLUGIN_SDK_ROOT}/include"
    INTERFACE_LINK_LIBRARIES "CAF::core;CAF::io;fmt::fmt;spdlog::spdlog"
)

if(WIN32)
    set_target_properties(caf_plugin_core PROPERTIES
        IMPORTED_LOCATION_DEBUG "${CAF_PLUGIN_SDK_ROOT}/lib/Debug/caf_plugin_core.dll"
        IMPORTED_IMPLIB_DEBUG   "${CAF_PLUGIN_SDK_ROOT}/lib/Debug/caf_plugin_core.lib"
        IMPORTED_LOCATION_RELEASE "${CAF_PLUGIN_SDK_ROOT}/lib/Release/caf_plugin_core.dll"
        IMPORTED_IMPLIB_RELEASE   "${CAF_PLUGIN_SDK_ROOT}/lib/Release/caf_plugin_core.lib"
        IMPORTED_LOCATION_RELWITHDEBINFO "${CAF_PLUGIN_SDK_ROOT}/lib/Release/caf_plugin_core.dll"
        IMPORTED_IMPLIB_RELWITHDEBINFO   "${CAF_PLUGIN_SDK_ROOT}/lib/Release/caf_plugin_core.lib"
    )
else()
    set_target_properties(caf_plugin_core PROPERTIES
        IMPORTED_LOCATION "${CAF_PLUGIN_SDK_ROOT}/lib/Release/caf_plugin_core.dll"
    )
endif()

# 规范别名：caf::plugin_core
if(NOT TARGET caf::plugin_core)
    add_library(caf::plugin_core ALIAS caf_plugin_core)
endif()
