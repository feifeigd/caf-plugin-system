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
