# src/cluster/CMakeLists.txt
# 集群模块（caf_plugin_cluster）—— master 扁平注册表 + 节点客户端
#
# 独立于插件框架（caf_plugin_core）：只依赖 CAF(core+io) 与 common 协议。
# 任何进程链接本库 + 调用 bootstrap_node 即成为集群节点。

add_library(caf_plugin_cluster STATIC
    master.cpp
    client.cpp
    bootstrap.cpp
)

target_include_directories(caf_plugin_cluster PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(caf_plugin_cluster PUBLIC CAF::core CAF::io)

if(WIN32)
    target_compile_options(caf_plugin_cluster PRIVATE /FS)
endif()
