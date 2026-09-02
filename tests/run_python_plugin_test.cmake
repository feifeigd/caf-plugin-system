foreach(required TEST_NAME RUNTIME_DIR APP_EXE PYTHON_STDLIB_SENTINEL SHARED_SCRIPT)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()
foreach(required_file "${APP_EXE}" "${PYTHON_STDLIB_SENTINEL}" "${SHARED_SCRIPT}")
  if(NOT EXISTS "${required_file}")
    message(FATAL_ERROR "Staged Python runtime input missing: ${required_file}")
  endif()
endforeach()

set(stdout_file "${CMAKE_CURRENT_BINARY_DIR}/${TEST_NAME}-stdout.log")
set(stderr_file "${CMAKE_CURRENT_BINARY_DIR}/${TEST_NAME}-stderr.log")
execute_process(
  COMMAND "${APP_EXE}"
    "--caf-plugin-system.entry-plugins=[\"PythonHostPlugin\"]"
    "--caf-plugin-system.test-auto-shutdown=true"
    "--caf-plugin-system.test-ctrl-c=false"
    "--caf-plugin-system.test-py-script=true"
  WORKING_DIRECTORY "${RUNTIME_DIR}"
  RESULT_VARIABLE result
  OUTPUT_FILE "${stdout_file}"
  ERROR_FILE "${stderr_file}"
  TIMEOUT 45)

file(READ "${stdout_file}" stdout)
file(READ "${stderr_file}" stderr)
if(NOT result EQUAL 0)
  message(FATAL_ERROR
    "${TEST_NAME} exited with ${result}\n--- stdout ---\n${stdout}\n--- stderr ---\n${stderr}")
endif()

foreach(marker
    "[PyTest] envelope call -> echo:1:ping"
    "[PyTest] string call -> echo-string:2:hello"
    "[PyTest] post-reload envelope call -> echo:3:ping"
    "Py_FinalizeEx done"
    "State: STOPPED"
    "framework shutdown complete")
  string(FIND "${stdout}" "${marker}" marker_pos)
  if(marker_pos EQUAL -1)
    message(FATAL_ERROR
      "${TEST_NAME} missing '${marker}'\n--- stdout ---\n${stdout}\n--- stderr ---\n${stderr}")
  endif()
endforeach()

message(STATUS "${TEST_NAME}: PASS")
