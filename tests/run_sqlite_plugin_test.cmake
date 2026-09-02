foreach(required TEST_NAME RUNTIME_DIR)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "${required} is missing")
  endif()
endforeach()

foreach(required APP_EXE CORE_DLL SQLITE_DLL THIRD_PARTY_DLL_DIR)
  if(NOT DEFINED ${required} OR NOT EXISTS "${${required}}")
    message(FATAL_ERROR "${required} is missing: '${${required}}'")
  endif()
endforeach()

file(REMOVE_RECURSE "${RUNTIME_DIR}")
file(MAKE_DIRECTORY "${RUNTIME_DIR}/plugins/sqlite")
file(COPY "${APP_EXE}" "${CORE_DLL}" DESTINATION "${RUNTIME_DIR}")
file(COPY "${SQLITE_DLL}" DESTINATION "${RUNTIME_DIR}/plugins/sqlite")

file(GLOB third_party_dlls "${THIRD_PARTY_DLL_DIR}/*.dll")
if(NOT third_party_dlls)
  message(FATAL_ERROR "No runtime DLLs found in ${THIRD_PARTY_DLL_DIR}")
endif()
file(COPY ${third_party_dlls} DESTINATION "${RUNTIME_DIR}")

file(WRITE "${RUNTIME_DIR}/caf-application.conf"
  "caf-plugin-system {\n"
  "  entry-plugins = [\"SqlitePlugin\"]\n"
  "  test-auto-shutdown = true\n"
  "  sqlite {\n"
  "    databases { default = \"./data/leak-test.db\" }\n"
  "    pool_size = 2\n"
  "    busy_timeout_ms = 5000\n"
  "  }\n"
  "}\n")

set(stdout_file "${RUNTIME_DIR}/stdout.log")
set(stderr_file "${RUNTIME_DIR}/stderr.log")
execute_process(
  COMMAND "${RUNTIME_DIR}/caf_plugin_app.exe"
  WORKING_DIRECTORY "${RUNTIME_DIR}"
  RESULT_VARIABLE result
  OUTPUT_FILE "${stdout_file}"
  ERROR_FILE "${stderr_file}"
  TIMEOUT 45
)

file(READ "${stdout_file}" stdout)
file(READ "${stderr_file}" stderr)
if(NOT result EQUAL 0)
  message(FATAL_ERROR
    "${TEST_NAME} exited with ${result}\n--- stdout ---\n${stdout}\n--- stderr ---\n${stderr}")
endif()

foreach(marker
    "SQLite selfcheck ok=true rows=1"
    "SqlitePlugin shutdown, 2 workers joined"
    "State: STOPPED"
    "framework shutdown complete")
  string(FIND "${stdout}" "${marker}" marker_pos)
  if(marker_pos EQUAL -1)
    message(FATAL_ERROR
      "${TEST_NAME} missing '${marker}'\n--- stdout ---\n${stdout}\n--- stderr ---\n${stderr}")
  endif()
endforeach()

string(FIND "${stderr}" "Detected memory leaks" leak_pos)
if(NOT leak_pos EQUAL -1)
  message(FATAL_ERROR
    "${TEST_NAME} reported CRT leaks\n--- stderr ---\n${stderr}")
endif()

message(STATUS
  "${TEST_NAME}: PASS (query ok, 2 workers joined, exit=0, no CRT leak report)")
