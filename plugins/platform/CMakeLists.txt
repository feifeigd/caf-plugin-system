# plugins/platform/CMakeLists.txt
# Platform 插件：提供多个基础设施服务（日志、配置、指标）

add_library(platform_plugin SHARED
    platform_plugin.cpp
)

target_link_libraries(platform_plugin PRIVATE caf_plugin_core)

# 定义插件名称（供 logging_service.hpp 的 LOG_* 宏使用）
target_compile_definitions(platform_plugin PRIVATE PLUGIN_NAME="platform")

set_target_properties(platform_plugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/platform"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/platform"
)

if(WIN32)
    set_target_properties(platform_plugin PROPERTIES PREFIX "")
else()
    set_target_properties(platform_plugin PROPERTIES PREFIX "lib")
endif()
