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

# ------------------------------------------------------------------
# 热更新演示目标：同一份源码编出 v2（BIZ_HOT_V2 切换行为差异作为
# "新代码已生效"的运行时证据），输出到独立目录。reload_atom 要求
# 新代码必须在【新路径】：Windows 对已加载 DLL 有文件锁，且
# LoadLibrary 对同路径直接返回缓存的旧模块（见 plugin_manager.cpp）。
# ------------------------------------------------------------------
add_library(business_plugin_v2 SHARED
    business_plugin.cpp
)

target_link_libraries(business_plugin_v2 PRIVATE caf_plugin_core)

target_compile_definitions(business_plugin_v2 PRIVATE PLUGIN_NAME="business" BIZ_HOT_V2=1)

set_target_properties(business_plugin_v2 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/business_v2"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/business_v2"
    OUTPUT_NAME "business_plugin_v2"
)

if(WIN32)
    set_target_properties(business_plugin_v2 PROPERTIES PREFIX "")
else()
    set_target_properties(business_plugin_v2 PROPERTIES PREFIX "lib")
endif()
