# 最小 libpq 泄露测试（std::thread 线程模型，同插件 worker）
find_package(PostgreSQL REQUIRED)
add_executable(pg_leak_test ${CMAKE_CURRENT_SOURCE_DIR}/../pg_leak_test.cpp)
target_link_libraries(pg_leak_test PRIVATE PostgreSQL::PostgreSQL)
set_target_properties(pg_leak_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
