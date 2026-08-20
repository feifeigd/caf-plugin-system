# plugins/business/CMakeLists.txt
# 构建 Business 插件（动态库）

add_library(business_plugin SHARED
    business_plugin.cpp
)

target_link_libraries(business_plugin PRIVATE caf_plugin_core)

# 定义插件名称（供 logging_service.hpp 的 LOG_* 宏使用）
target_compile_definitions(business_plugin PRIVATE PLUGIN_NAME="business")

set_target_properties(business_plugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/business"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/business"
)

if(WIN32)
    set_target_properties(business_plugin PROPERTIES PREFIX "")
else()
    set_target_properties(business_plugin PROPERTIES PREFIX "lib")
endif()
