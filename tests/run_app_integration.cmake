if(NOT DEFINED TEST_NAME OR NOT DEFINED TEST_MODE OR NOT DEFINED RUNTIME_DIR)
  message(FATAL_ERROR "TEST_NAME, TEST_MODE and RUNTIME_DIR are required")
endif()

foreach(required APP_EXE CORE_DLL BUSINESS_DLL BUSINESS_V2_DLL THIRD_PARTY_DLL_DIR)
  if(NOT DEFINED ${required} OR NOT EXISTS "${${required}}")
    message(FATAL_ERROR "${required} is missing: '${${required}}'")
  endif()
endforeach()

# 每个测试使用独立运行目录，避免依赖手工维护的 run/ 和旧二进制。
file(REMOVE_RECURSE "${RUNTIME_DIR}")
file(MAKE_DIRECTORY "${RUNTIME_DIR}/plugins/business" "${RUNTIME_DIR}/updates")
file(COPY "${APP_EXE}" "${CORE_DLL}" DESTINATION "${RUNTIME_DIR}")
file(COPY "${BUSINESS_DLL}"
     DESTINATION "${RUNTIME_DIR}/plugins/business")
file(COPY "${BUSINESS_V2_DLL}" DESTINATION "${RUNTIME_DIR}/updates")

file(GLOB third_party_dlls "${THIRD_PARTY_DLL_DIR}/*.dll")
if(NOT third_party_dlls)
  message(FATAL_ERROR "No runtime DLLs found in ${THIRD_PARTY_DLL_DIR}")
endif()
file(COPY ${third_party_dlls} DESTINATION "${RUNTIME_DIR}")

set(config "caf-plugin-system {\n  entry-plugins = [\"BusinessPlugin\"]\n")
if(TEST_MODE STREQUAL "auto_shutdown")
  string(APPEND config "  test-auto-shutdown = true\n")
  set(test_args "--caf-plugin-system.test-auto-shutdown=true")
elseif(TEST_MODE STREQUAL "ctrl_c")
  string(APPEND config "  test-auto-shutdown = false\n")
  set(test_args "--caf-plugin-system.test-ctrl-c=true")
else()
  message(FATAL_ERROR "Unknown TEST_MODE: ${TEST_MODE}")
endif()
string(APPEND config "}\n")
file(WRITE "${RUNTIME_DIR}/caf-application.conf" "${config}")

set(stdout_file "${RUNTIME_DIR}/stdout.log")
set(stderr_file "${RUNTIME_DIR}/stderr.log")
execute_process(
  COMMAND "${RUNTIME_DIR}/caf_plugin_app.exe" ${test_args}
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

foreach(marker "State: STOPPED" "framework shutdown complete")
  string(FIND "${stdout}" "${marker}" marker_pos)
  if(marker_pos EQUAL -1)
    message(FATAL_ERROR
      "${TEST_NAME} missing '${marker}'\n--- stdout ---\n${stdout}\n--- stderr ---\n${stderr}")
  endif()
endforeach()

if(TEST_MODE STREQUAL "auto_shutdown")
  foreach(marker "[HotUpdate] reload result: 1" "processed by v2:")
    string(FIND "${stdout}" "${marker}" marker_pos)
    if(marker_pos EQUAL -1)
      message(FATAL_ERROR
        "${TEST_NAME} missing '${marker}'\n--- stdout ---\n${stdout}\n--- stderr ---\n${stderr}")
    endif()
  endforeach()
else()
  string(FIND "${stdout}" "simulating Ctrl+C" marker_pos)
  if(marker_pos EQUAL -1)
    message(FATAL_ERROR
      "${TEST_NAME} did not exercise Ctrl+C path\n--- stdout ---\n${stdout}")
  endif()
endif()

string(FIND "${stderr}" "Detected memory leaks" leak_pos)
if(NOT leak_pos EQUAL -1)
  message(FATAL_ERROR
    "${TEST_NAME} reported CRT leaks\n--- stderr ---\n${stderr}")
endif()

message(STATUS "${TEST_NAME}: PASS (exit=0, graceful shutdown, no CRT leak report)")
