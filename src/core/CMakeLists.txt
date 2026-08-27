# src/core/CMakeLists.txt
# 核心动态库 caf_plugin_core（方案 B，2026-08-27：STATIC → SHARED）——
# 一个库，内部分层：
#   plugin/   插件子系统（manager/loader/dynamic_library/lifecycle/interface）
#   cluster/  集群子系统（master 注册表/节点客户端/引导）
#   根        内核服务（registry/checkpoint/graceful_shutdown/框架引导）
#
# SHARED 的意义：exe 与各插件 DLL 链接同一动态库 → 库内全局存储
#（g_time_offset、current_logger 等）进程级单一实体，跨模块天然共享，
# 无"每模块副本 + 同步推送"税（旧 STATIC + inline 变量的 Windows
# DLL 边界坑，见 include/common/core_export.hpp 与 time_service.hpp）。

add_library(caf_plugin_core SHARED
    # ---- 内核服务 ----
    logging_service.cpp
    service_registry.cpp
    checkpoint_manager.cpp
    graceful_shutdown.cpp
    framework_bootstrap.cpp
    time_service.cpp
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
    # ---- bridge（外部语言节点 sidecar）----
    bridge_actor.cpp
)

target_include_directories(caf_plugin_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# cluster 需要 CAF::io（middleman）；日志服务需要 spdlog（sinks 归 logging_service）
target_link_libraries(caf_plugin_core PUBLIC CAF::core CAF::io fmt::fmt spdlog::spdlog)

if(WIN32)
    # 自动导出全部非 inline 符号（普通函数/类成员免手标 CORE_API；
    # inline 变量如 g_time_offset 仍须显式 CORE_API，见 core_export.hpp）
    set_target_properties(caf_plugin_core PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
    target_compile_options(caf_plugin_core PRIVATE /FS)
    target_link_libraries(caf_plugin_core PRIVATE kernel32)
endif()
