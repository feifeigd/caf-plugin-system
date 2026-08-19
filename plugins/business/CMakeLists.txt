# plugins/business/CMakeLists.txt
# 构建 Business 插件（动态库）

add_library(business_plugin SHARED
    business_plugin.cpp
)

target_link_libraries(business_plugin PRIVATE caf_plugin_core)

set_target_properties(business_plugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/business"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/business"
)

if(WIN32)
    set_target_properties(business_plugin PROPERTIES PREFIX "")
else()
    set_target_properties(business_plugin PROPERTIES PREFIX "lib")
endif()
