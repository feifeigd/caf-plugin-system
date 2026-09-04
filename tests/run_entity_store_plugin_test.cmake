foreach(required TEST_NAME RUNTIME_DIR)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "${required} is missing")
  endif()
endforeach()

if(NOT DEFINED BACKEND)
  set(BACKEND sqlite)
endif()
if(BACKEND STREQUAL "sqlite")
  set(backend_plugin SqlitePlugin)
  set(backend_service sqlite_service)
  if(NOT DEFINED BACKEND_DLL)
    set(BACKEND_DLL "${SQLITE_DLL}")
  endif()
  set(backend_config
    "  sqlite {\n    databases {\n      reader = \"./data/entity-store.db\"\n      writer = \"./data/entity-store.db\"\n    }\n    pool_size = 2\n    busy_timeout_ms = 5000\n  }\n")
elseif(BACKEND STREQUAL "mysql" OR BACKEND STREQUAL "postgres")
  if(BACKEND STREQUAL "mysql")
    set(backend_plugin MySqlPlugin)
    set(backend_service mysql_service)
  else()
    set(backend_plugin PostgresPlugin)
    set(backend_service pg_service)
  endif()
  set(database_uri "$ENV{CAF_ENTITY_TEST_DB_URI}")
  if(database_uri STREQUAL "" OR database_uri MATCHES "[\"\n\r]")
    message(FATAL_ERROR "CAF_ENTITY_TEST_DB_URI is missing or invalid")
  endif()
  set(admin_uri "$ENV{CAF_ENTITY_TEST_ADMIN_URI}")
  if(admin_uri STREQUAL "" OR admin_uri MATCHES "[\"\n\r]")
    message(FATAL_ERROR "CAF_ENTITY_TEST_ADMIN_URI is missing or invalid")
  endif()
  set(backend_config
    "  ${BACKEND} {\n    uris {\n      reader = \"${database_uri}\"\n      writer = \"${database_uri}\"\n      recovery = \"${database_uri}\"\n      recovery_admin = \"${admin_uri}\"\n    }\n    pool_size = 1\n  }\n")
else()
  message(FATAL_ERROR "Unsupported EntityStore test backend: ${BACKEND}")
endif()

foreach(required APP_EXE CORE_DLL BACKEND_DLL ENTITY_STORE_DLL THIRD_PARTY_DLL_DIR)
  if(NOT DEFINED ${required} OR NOT EXISTS "${${required}}")
    message(FATAL_ERROR "${required} is missing: '${${required}}'")
  endif()
endforeach()

# 只允许重建仓库 out/build 下的测试产物，禁止参数错误删除源码/工作区。
get_filename_component(runtime_absolute "${RUNTIME_DIR}" ABSOLUTE)
get_filename_component(build_root "${CMAKE_CURRENT_LIST_DIR}/../out/build" ABSOLUTE)
file(TO_CMAKE_PATH "${runtime_absolute}" runtime_normalized)
file(TO_CMAKE_PATH "${build_root}" build_normalized)
string(TOLOWER "${runtime_normalized}/" runtime_lower)
string(TOLOWER "${build_normalized}/" build_lower)
string(FIND "${runtime_lower}" "${build_lower}" build_prefix)
if(NOT build_prefix EQUAL 0 OR runtime_lower STREQUAL build_lower)
  message(FATAL_ERROR "Unsafe test runtime directory: ${RUNTIME_DIR}")
endif()
set(RUNTIME_DIR "${runtime_absolute}")
file(REMOVE_RECURSE "${RUNTIME_DIR}")
file(MAKE_DIRECTORY
  "${RUNTIME_DIR}/plugins/${BACKEND}"
  "${RUNTIME_DIR}/plugins/entity_store")
file(COPY "${APP_EXE}" "${CORE_DLL}" DESTINATION "${RUNTIME_DIR}")
file(COPY "${BACKEND_DLL}" DESTINATION "${RUNTIME_DIR}/plugins/${BACKEND}")
file(COPY "${ENTITY_STORE_DLL}"
     DESTINATION "${RUNTIME_DIR}/plugins/entity_store")

file(GLOB third_party_dlls "${THIRD_PARTY_DLL_DIR}/*.dll")
if(NOT third_party_dlls)
  message(FATAL_ERROR "No runtime DLLs found in ${THIRD_PARTY_DLL_DIR}")
endif()
file(COPY ${third_party_dlls} DESTINATION "${RUNTIME_DIR}")
if(BACKEND STREQUAL "mysql")
  set(SOURCE_BIN_DIR "${THIRD_PARTY_DLL_DIR}")
  set(DEST_DIR "${RUNTIME_DIR}")
  include("${CMAKE_CURRENT_LIST_DIR}/../cmake/copy_mariadb_auth_plugins.cmake")
  if(NOT DEFINED MYSQL_CANCELLATION_EXE OR NOT EXISTS "${MYSQL_CANCELLATION_EXE}")
    message(FATAL_ERROR "MYSQL_CANCELLATION_EXE is missing")
  endif()
  file(COPY "${MYSQL_CANCELLATION_EXE}" DESTINATION "${RUNTIME_DIR}")
endif()

file(WRITE "${RUNTIME_DIR}/caf-application.conf"
  "caf-plugin-system {\n"
  "  entry-plugins = [\"EntityStorePlugin\", \"${backend_plugin}\"]\n"
  "  test-entity-store = true\n"
  "${backend_config}"
  "  entity_store {\n"
  "    dialect = \"${BACKEND}\"\n"
  "    backend_service = \"${backend_service}\"\n"
  "    request_timeout_ms = 10000\n"
  "    stores {\n"
  "      commerce {\n"
  "        read_connection = \"reader\"\n"
  "        write_connection = \"writer\"\n"
  "        entities {\n"
  "          order {\n"
  "            table = \"orders\"\n"
  "            version_column = \"version\"\n"
  "            keys {\n"
  "              order_id {\n"
  "                column = \"order_id\"\n"
  "                kind = \"text\"\n"
  "                nullable = false\n"
  "              }\n"
  "            }\n"
  "            fields {\n"
  "              status {\n"
  "                column = \"status\"\n"
  "                kind = \"text\"\n"
  "                nullable = false\n"
  "              }\n"
  "              paid_cents {\n"
  "                column = \"paid_cents\"\n"
  "                kind = \"signed_integer\"\n"
  "                nullable = false\n"
  "              }\n"
  "              memo {\n"
  "                column = \"memo\"\n"
  "                kind = \"text\"\n"
  "                nullable = true\n"
  "              }\n"
  "            }\n"
  "          }\n"
  "          payment {\n"
  "            table = \"payments\"\n"
  "            version_column = \"version\"\n"
  "            keys {\n"
  "              payment_id {\n"
  "                column = \"payment_id\"\n"
  "                kind = \"text\"\n"
  "                nullable = false\n"
  "              }\n"
  "            }\n"
  "            fields {\n"
  "              state {\n"
  "                column = \"state\"\n"
  "                kind = \"text\"\n"
  "                nullable = false\n"
  "              }\n"
  "            }\n"
  "          }\n"
  "        }\n"
  "      }\n"
  "    }\n"
  "  }\n"
  "}\n")

file(READ "${RUNTIME_DIR}/caf-application.conf" manual_configuration)
# The manual pass creates the real business tables. The database pass starts a
# new application against those tables and supplies no key/field schema at all.
# Runtime/data is retained between passes; containers remain strictly sequential.
string(CONCAT database_configuration
  "caf-plugin-system {\n"
  "  entry-plugins = [\"EntityStorePlugin\", \"${backend_plugin}\"]\n"
  "  test-entity-store = true\n"
  "${backend_config}"
  "  entity_store {\n"
  "    dialect = \"${BACKEND}\"\n"
  "    backend_service = \"${backend_service}\"\n"
  "    schema_source = \"database\"\n"
  "    request_timeout_ms = 10000\n"
  "    stores {\n"
  "      commerce {\n"
  "        read_connection = \"reader\"\n"
  "        write_connection = \"writer\"\n"
  "        entities {\n"
  "          order {\n            table = \"orders\"\n          }\n"
  "          payment {\n            table = \"payments\"\n          }\n"
  "        }\n      }\n    }\n  }\n}\n")

foreach(schema_mode manual database)
if(schema_mode STREQUAL "manual")
  file(WRITE "${RUNTIME_DIR}/caf-application.conf" "${manual_configuration}")
else()
  file(WRITE "${RUNTIME_DIR}/caf-application.conf" "${database_configuration}")
endif()
set(stdout_file "${RUNTIME_DIR}/${schema_mode}-stdout.log")
set(stderr_file "${RUNTIME_DIR}/${schema_mode}-stderr.log")
execute_process(
  COMMAND "${RUNTIME_DIR}/caf_plugin_app.exe"
  WORKING_DIRECTORY "${RUNTIME_DIR}"
  RESULT_VARIABLE result
  OUTPUT_FILE "${stdout_file}"
  ERROR_FILE "${stderr_file}"
  TIMEOUT 45
)

# Do not retain temporary Docker credentials after the application exits.
if(NOT BACKEND STREQUAL "sqlite")
  file(REMOVE "${RUNTIME_DIR}/caf-application.conf")
endif()
file(READ "${stdout_file}" stdout)
file(READ "${stderr_file}" stderr)
if(NOT result EQUAL 0)
  message(FATAL_ERROR
    "${TEST_NAME} exited with ${result}\n--- stdout ---\n${stdout}\n--- stderr ---\n${stderr}")
endif()

foreach(marker
    "[EntityStoreTest] PASS"
    "${backend_plugin} shutdown, 4 workers joined"
    "State: STOPPED"
    "framework shutdown complete")
  string(FIND "${stdout}" "${marker}" marker_pos)
  if(marker_pos EQUAL -1)
    message(FATAL_ERROR
      "${TEST_NAME} missing '${marker}'\n--- stdout ---\n${stdout}\n--- stderr ---\n${stderr}")
  endif()
endforeach()

string(FIND "${stdout}" "[EntityStoreTest] FAIL" failure_pos)
if(NOT failure_pos EQUAL -1)
  message(FATAL_ERROR
    "${TEST_NAME} reported failure\n--- stdout ---\n${stdout}\n--- stderr ---\n${stderr}")
endif()

string(FIND "${stderr}" "Detected memory leaks" leak_pos)
if(NOT leak_pos EQUAL -1)
  message(FATAL_ERROR
    "${TEST_NAME} reported CRT leaks\n--- stderr ---\n${stderr}")
endif()

message(STATUS
  "${TEST_NAME}/${schema_mode}: PASS (EntityStore + ${BACKEND} E2E, graceful shutdown, no CRT leak report)")
endforeach()

if(BACKEND STREQUAL "mysql")
  execute_process(
    COMMAND "${RUNTIME_DIR}/test_mysql_cancellation.exe"
    WORKING_DIRECTORY "${RUNTIME_DIR}"
    RESULT_VARIABLE cancellation_result
    OUTPUT_FILE "${RUNTIME_DIR}/cancellation-stdout.log"
    ERROR_FILE "${RUNTIME_DIR}/cancellation-stderr.log"
    TIMEOUT 60
  )
  file(READ "${RUNTIME_DIR}/cancellation-stdout.log" cancellation_stdout)
  file(READ "${RUNTIME_DIR}/cancellation-stderr.log" cancellation_stderr)
  string(FIND "${cancellation_stdout}" "[MySqlCancellationTest] PASS natural exit" cancellation_pass)
  if(NOT cancellation_result EQUAL 0
     OR cancellation_pass EQUAL -1
     OR cancellation_stderr MATCHES "Detected memory leaks")
    message(FATAL_ERROR
      "${TEST_NAME}/cancellation failed (${cancellation_result})\n${cancellation_stdout}\n${cancellation_stderr}")
  endif()
  message(STATUS "${TEST_NAME}/cancellation: PASS (bounded stop, worker joined, no CRT leak report)")
endif()
