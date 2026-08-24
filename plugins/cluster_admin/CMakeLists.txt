# plugins/cluster_admin/CMakeLists.txt
# 构建 ClusterAdmin 插件（集群管理：system.nodes/services/ping）

add_library(cluster_admin_plugin SHARED
    cluster_admin_plugin.cpp
)

target_link_libraries(cluster_admin_plugin PRIVATE caf_plugin_core)

# 定义插件名称（供 logging_service.hpp 的 LOG_* 宏使用）
target_compile_definitions(cluster_admin_plugin PRIVATE PLUGIN_NAME="cluster_admin")

set_target_properties(cluster_admin_plugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/cluster_admin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/cluster_admin"
)

if(WIN32)
    set_target_properties(cluster_admin_plugin PROPERTIES PREFIX "")
else()
    set_target_properties(cluster_admin_plugin PROPERTIES PREFIX "lib")
endif()
