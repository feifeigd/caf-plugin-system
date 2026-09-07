# tests/CMakeLists.txt
# 构建测试可执行文件

add_executable(test_dependency_graph
    test_dependency_graph.cpp
)

# 测试直接链接核心库（无需重复找 CAF，已通过 caf_plugin_core 传递）
target_link_libraries(test_dependency_graph PRIVATE caf_plugin_core)

# 注册为 CTest 测试
add_test(NAME dependency_graph COMMAND test_dependency_graph)

add_executable(test_entity_store_contract
    test_entity_store_contract.cpp
)
target_link_libraries(test_entity_store_contract PRIVATE caf_plugin_core)
add_test(NAME entity_store_contract COMMAND test_entity_store_contract)

add_executable(test_sql_connection_pool
    test_sql_connection_pool.cpp
)
target_link_libraries(test_sql_connection_pool PRIVATE caf_plugin_core)
add_test(NAME sql_connection_pool COMMAND test_sql_connection_pool)

# This layer deliberately has no CAF or database-driver dependency.
find_package(Threads REQUIRED)
add_executable(test_db_worker_pool test_db_worker_pool.cpp)
target_include_directories(test_db_worker_pool PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(test_db_worker_pool PRIVATE Threads::Threads)
add_test(NAME db_worker_pool COMMAND test_db_worker_pool)
set_tests_properties(db_worker_pool PROPERTIES TIMEOUT 30)

add_executable(test_sql_entity_store
    test_sql_entity_store.cpp
)
target_link_libraries(test_sql_entity_store PRIVATE caf_plugin_core)
add_test(NAME sql_entity_store COMMAND test_sql_entity_store)

foreach(mongo_test mongo_entity_store_schema mongo_entity_store_actor)
    add_executable(test_${mongo_test} test_${mongo_test}.cpp)
    target_link_libraries(test_${mongo_test} PRIVATE caf_plugin_core)
    add_test(NAME ${mongo_test} COMMAND test_${mongo_test})
    set_tests_properties(${mongo_test} PROPERTIES TIMEOUT 30)
endforeach()

foreach(sql_test sql_schema_provider sql_reconnecting_worker entity_store_recovery entity_store_schema_startup)
    add_executable(test_${sql_test} test_${sql_test}.cpp)
    target_link_libraries(test_${sql_test} PRIVATE caf_plugin_core)
    add_test(NAME ${sql_test} COMMAND test_${sql_test})
    set_tests_properties(${sql_test} PROPERTIES TIMEOUT 30)
endforeach()

# Native BSON inspection plus two dynamically loaded production plugins.
# The executable requires an isolated replica set; only the Docker runner invokes it.
find_package(mongocxx CONFIG REQUIRED)
add_executable(test_mongo_entity_store test_mongo_entity_store.cpp)
if(TARGET mongo::mongocxx_shared)
    target_link_libraries(test_mongo_entity_store PRIVATE caf_plugin_core mongo::mongocxx_shared)
else()
    target_link_libraries(test_mongo_entity_store PRIVATE caf_plugin_core mongo::mongocxx_static)
endif()
add_dependencies(test_mongo_entity_store mongo_plugin entity_store_plugin)

add_executable(test_redis_worker_pool test_redis_worker_pool.cpp)
target_link_libraries(test_redis_worker_pool PRIVATE caf_plugin_core)
add_dependencies(test_redis_worker_pool redis_plugin)

add_executable(test_redis_entity_store test_redis_entity_store.cpp)
target_link_libraries(test_redis_entity_store PRIVATE caf_plugin_core)
add_dependencies(test_redis_entity_store redis_plugin entity_store_plugin)

# Socket lifetime/cancellation tests do not require a database or Docker.
add_executable(test_sql_socket_cancellation test_sql_socket_cancellation.cpp)
target_link_libraries(test_sql_socket_cancellation PRIVATE caf_plugin_core)
if(WIN32)
    target_link_libraries(test_sql_socket_cancellation PRIVATE ws2_32)
endif()
add_test(NAME sql_socket_cancellation COMMAND test_sql_socket_cancellation)
set_tests_properties(sql_socket_cancellation PROPERTIES TIMEOUT 30)

# Executed by the Docker runner against its existing isolated MySQL container.
# Include the real driver implementation in this test so its worker I/O path is
# exercised without adding a production-only test message or application hook.
find_package(unofficial-libmariadb CONFIG REQUIRED)
add_executable(test_mysql_cancellation test_mysql_cancellation.cpp)
target_include_directories(test_mysql_cancellation PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(test_mysql_cancellation PRIVATE caf_plugin_core unofficial::libmariadb)
target_compile_definitions(test_mysql_cancellation PRIVATE PLUGIN_NAME="mysql-cancellation-test")
if(WIN32)
    target_link_libraries(test_mysql_cancellation PRIVATE ws2_32)
endif()

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

    add_test(NAME sqlite_plugin_leak
        COMMAND "${CMAKE_COMMAND}"
            "-DTEST_NAME=sqlite_plugin_leak"
            "-DRUNTIME_DIR=${CMAKE_CURRENT_BINARY_DIR}/sqlite_plugin_leak-$<CONFIG>"
            "-DAPP_EXE=$<TARGET_FILE:caf_plugin_app>"
            "-DCORE_DLL=$<TARGET_FILE:caf_plugin_core>"
            "-DSQLITE_DLL=$<TARGET_FILE:sqlite_plugin>"
            "-DTHIRD_PARTY_DLL_DIR=${vcpkg_runtime_dir}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_sqlite_plugin_test.cmake"
    )
    set_tests_properties(sqlite_plugin_leak PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 60
    )

    add_test(NAME entity_store_plugin_e2e
        COMMAND "${CMAKE_COMMAND}"
            "-DTEST_NAME=entity_store_plugin_e2e"
            "-DRUNTIME_DIR=${CMAKE_CURRENT_BINARY_DIR}/entity_store_plugin_e2e-$<CONFIG>"
            "-DAPP_EXE=$<TARGET_FILE:caf_plugin_app>"
            "-DCORE_DLL=$<TARGET_FILE:caf_plugin_core>"
            "-DSQLITE_DLL=$<TARGET_FILE:sqlite_plugin>"
            "-DENTITY_STORE_DLL=$<TARGET_FILE:entity_store_plugin>"
            "-DTHIRD_PARTY_DLL_DIR=${vcpkg_runtime_dir}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_entity_store_plugin_test.cmake"
    )
    set_tests_properties(entity_store_plugin_e2e PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 60
    )

    # Opt in explicitly: normal unit tests do not require Docker Desktop.
    option(CAF_ENABLE_DOCKER_TESTS "Run real database tests using temporary Docker containers" OFF)
    if(CAF_ENABLE_DOCKER_TESTS)
        find_program(ENTITY_STORE_PWSH NAMES pwsh REQUIRED)
        add_test(NAME entity_store_docker
            COMMAND "${ENTITY_STORE_PWSH}" -NoProfile -File
                "${CMAKE_CURRENT_SOURCE_DIR}/run_entity_store_docker.ps1"
                -BuildDir "${CMAKE_BINARY_DIR}" -Configuration "$<CONFIG>"
        )
        set_tests_properties(entity_store_docker PROPERTIES
            RUN_SERIAL TRUE
            TIMEOUT 600
        )
        add_test(NAME mongo_entity_store_docker
            COMMAND "${ENTITY_STORE_PWSH}" -NoProfile -File
                "${CMAKE_CURRENT_SOURCE_DIR}/run_mongo_entity_store_docker.ps1"
                -BuildDir "${CMAKE_BINARY_DIR}" -Configuration "$<CONFIG>"
        )
        set_tests_properties(mongo_entity_store_docker PROPERTIES
            RUN_SERIAL TRUE
            TIMEOUT 300
        )
        add_test(NAME redis_worker_pool_docker
            COMMAND "${ENTITY_STORE_PWSH}" -NoProfile -File
                "${CMAKE_CURRENT_SOURCE_DIR}/run_redis_worker_pool_docker.ps1"
                -BuildDir "${CMAKE_BINARY_DIR}" -Configuration "$<CONFIG>"
        )
        set_tests_properties(redis_worker_pool_docker PROPERTIES RUN_SERIAL TRUE TIMEOUT 180)
        add_test(NAME redis_entity_store_docker
            COMMAND "${ENTITY_STORE_PWSH}" -NoProfile -File
                "${CMAKE_CURRENT_SOURCE_DIR}/run_redis_entity_store_docker.ps1"
                -BuildDir "${CMAKE_BINARY_DIR}" -Configuration "$<CONFIG>"
        )
        set_tests_properties(redis_entity_store_docker PROPERTIES RUN_SERIAL TRUE TIMEOUT 240)
    endif()
endif()

# ------------------------------------------------------------------
# Python 插件端到端回归：从最终 staging 目录加载解释器/标准库/共享脚本，
# 覆盖 envelope、string、热重载状态交接与优雅关机。
# ------------------------------------------------------------------
if(WIN32 OR UNIX)
    if(WIN32)
        set(python_stdlib_sentinel "${APP_RUN_DIR}/Lib/encodings/__init__.py")
    else()
        set(python_stdlib_sentinel
            "${APP_RUN_DIR}/lib/python${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}/encodings/__init__.py")
    endif()
    add_test(NAME python_plugin_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DTEST_NAME=python_plugin_smoke"
            "-DRUNTIME_DIR=${APP_RUN_DIR}"
            "-DAPP_EXE=${APP_RUN_DIR}/caf_plugin_app${CMAKE_EXECUTABLE_SUFFIX}"
            "-DPYTHON_STDLIB_SENTINEL=${python_stdlib_sentinel}"
            "-DSHARED_SCRIPT=${APP_RUN_ROOT}/shared/scripts/python/echo.py"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_python_plugin_test.cmake"
    )
    set_tests_properties(python_plugin_smoke PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 60
    )
endif()

# ------------------------------------------------------------------
# 最小 libpq 泄露测试（独立 exe，不链接 caf，纯 PQconnectdb+PQfinish）
# ------------------------------------------------------------------
add_subdirectory(pg_leak_test)
