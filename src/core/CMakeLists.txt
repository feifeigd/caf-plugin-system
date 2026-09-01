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
    cluster/remote_lookup_worker.cpp
    cluster/remote_caller.cpp
    cluster/ops_actor.cpp
    # ---- bridge（外部语言节点 sidecar）----
    bridge_actor.cpp
    # ---- Pomelo 协议端点（游戏客户端接入，TCP/WS 双传输）----
    pomelo_bridge.cpp
    pomelo_ws.cpp
    # ---- 应用入口（dll_main 导出，exe 瘦壳 LoadLibrary 后调用）----
    __main__.cpp
)

# ---- 验证后门（--test-*）----
# 2026-08-31：测试代码移回 app（exe 编译）。core 不再含测试代码：
# 改测试只重编 exe，core.dll 与全部插件 DLL 均不重建（插件全链
# 链接 caf_plugin_core，core 重编 = 全链重编，必须避免）。
# core 的 dll_main 经 test_hook 函数指针调用 exe 侧的 run_test_backdoors，
# 无动态加载、无 DLL 生命周期（2026-08-31 崩溃根因：test DLL 卸载后
# delayed 线程执行已卸载代码段 → 0xC0000005）。

target_include_directories(caf_plugin_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/test>
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

# CAF 最小关机测试（2026-08-31）：判定关机崩溃归属（CAF vs 我们的流程）
add_executable(caf_minimal_test caf_minimal_test.cpp)
target_include_directories(caf_minimal_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(caf_minimal_test PRIVATE CAF::core)
