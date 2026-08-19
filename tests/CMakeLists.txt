# tests/CMakeLists.txt
# 构建测试可执行文件

add_executable(test_dependency_graph
    test_dependency_graph.cpp
)

# 测试直接链接核心库（无需重复找 CAF，已通过 caf_plugin_core 传递）
target_link_libraries(test_dependency_graph PRIVATE caf_plugin_core)

# 注册为 CTest 测试
add_test(NAME dependency_graph COMMAND test_dependency_graph)
