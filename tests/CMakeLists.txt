# tests/CMakeLists.txt
# 构建测试可执行文件

add_executable(test_dependency_graph
    test_dependency_graph.cpp
)

# 测试直接链接核心库（无需重复找 CAF，已通过 caf_plugin_core 传递）
target_link_libraries(test_dependency_graph PRIVATE caf_plugin_core)

# 注册为 CTest 测试
add_test(NAME dependency_graph COMMAND test_dependency_graph)

# ------------------------------------------------------------------
# typed_actor 跨 DLL 冒烟验证（评估 typed 重构可行性的最小试验田）
# 结构：typed_smoke_plugin.dll（模拟插件，实现 typed 服务）
#       + typed_smoke.exe（cast + untyped 代理转发 + 全链路验证）
# ------------------------------------------------------------------
add_library(typed_smoke_plugin SHARED
    typed_smoke/typed_smoke_plugin.cpp
)
target_include_directories(typed_smoke_plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/typed_smoke
)
target_link_libraries(typed_smoke_plugin PRIVATE caf_plugin_core)

add_executable(typed_smoke
    typed_smoke/typed_smoke_main.cpp
)
target_include_directories(typed_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/typed_smoke
)
target_link_libraries(typed_smoke PRIVATE caf_plugin_core typed_smoke_plugin)

add_test(NAME typed_smoke COMMAND typed_smoke)

# ------------------------------------------------------------------
# 应用级集成回归：隔离组装运行目录，不依赖手工同步的 run/。
# 覆盖插件加载/ACL/热更新/状态保存/统一关机，以及 Ctrl+C 等价路径。
# ------------------------------------------------------------------
if(WIN32)
    set(app_integration_script
        "${CMAKE_CURRENT_SOURCE_DIR}/run_app_integration.cmake")
    set(vcpkg_runtime_dir
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<IF:$<CONFIG:Debug>,debug/bin,bin>")

    foreach(mode auto_shutdown ctrl_c)
        set(test_name "app_${mode}")
        add_test(NAME "${test_name}"
            COMMAND "${CMAKE_COMMAND}"
                "-DTEST_NAME=${test_name}"
                "-DTEST_MODE=${mode}"
                "-DRUNTIME_DIR=${CMAKE_CURRENT_BINARY_DIR}/${test_name}-$<CONFIG>"
                "-DAPP_EXE=$<TARGET_FILE:caf_plugin_app>"
                "-DCORE_DLL=$<TARGET_FILE:caf_plugin_core>"
                "-DBUSINESS_DLL=$<TARGET_FILE:business_plugin>"
                "-DBUSINESS_V2_DLL=$<TARGET_FILE:business_plugin_v2>"
                "-DTHIRD_PARTY_DLL_DIR=${vcpkg_runtime_dir}"
                -P "${app_integration_script}"
        )
        set_tests_properties("${test_name}" PROPERTIES
            RUN_SERIAL TRUE
            TIMEOUT 60
        )
    endforeach()
endif()

# ------------------------------------------------------------------
# 最小 libpq 泄露测试（独立 exe，不链接 caf，纯 PQconnectdb+PQfinish）
# ------------------------------------------------------------------
add_subdirectory(pg_leak_test)
