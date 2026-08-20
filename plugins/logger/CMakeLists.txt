# plugins/logger/CMakeLists.txt
# 构建 Logger 插件（动态库）

add_library(logger_plugin SHARED
    logger_plugin.cpp
)

target_link_libraries(logger_plugin PRIVATE caf_plugin_core spdlog::spdlog)

# 定义插件名称（供 logging_service.hpp 的 LOG_* 宏使用）
target_compile_definitions(logger_plugin PRIVATE PLUGIN_NAME="logger")

set_target_properties(logger_plugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/logger"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/logger"
)

if(WIN32)
    set_target_properties(logger_plugin PROPERTIES PREFIX "")
else()
    set_target_properties(logger_plugin PROPERTIES PREFIX "lib")
endif()
