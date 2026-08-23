# src/core/CMakeLists.txt
# 核心静态库 caf_plugin_core —— 一个库，内部分层：
#   plugin/   插件子系统（manager/loader/dynamic_library/lifecycle/interface）
#   cluster/  集群子系统（master 注册表/节点客户端/引导）
#   根        内核服务（registry/checkpoint/graceful_shutdown/框架引导）

add_library(caf_plugin_core STATIC
    # ---- 内核服务 ----
    logging_service.cpp
    service_registry.cpp
    checkpoint_manager.cpp
    graceful_shutdown.cpp
    framework_bootstrap.cpp
    # ---- 插件子系统 ----
    plugin/plugin_manager.cpp
    plugin/plugin_loader.cpp
    plugin/dynamic_library.cpp
    # ---- 集群子系统 ----
    cluster/master.cpp
    cluster/client.cpp
    cluster/bootstrap.cpp
    cluster/remote_caller.cpp
    cluster/ops_actor.cpp
)

target_include_directories(caf_plugin_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# cluster 需要 CAF::io（middleman）；日志服务需要 spdlog（sinks 归 logging_service）
target_link_libraries(caf_plugin_core PUBLIC CAF::core CAF::io fmt::fmt spdlog::spdlog)

if(WIN32)
    target_compile_options(caf_plugin_core PRIVATE /FS)
    target_link_libraries(caf_plugin_core PRIVATE kernel32)
endif()
