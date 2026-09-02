foreach(required UV_EXECUTABLE PYTHON_EXECUTABLE PROJECT_DIR TARGET_DIR CONFIGURATION PLATFORM_KIND)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

foreach(required_file
        "${PROJECT_DIR}/pyproject.toml" "${PROJECT_DIR}/uv.lock"
        "${UV_EXECUTABLE}" "${PYTHON_EXECUTABLE}")
  if(NOT EXISTS "${required_file}")
    message(FATAL_ERROR "Required uv input not found: ${required_file}")
  endif()
endforeach()

file(SHA256 "${PROJECT_DIR}/pyproject.toml" project_hash)
file(SHA256 "${PROJECT_DIR}/uv.lock" lock_hash)
set(fingerprint "${project_hash}:${lock_hash}:${CONFIGURATION}:${PLATFORM_KIND}")
set(stamp "${TARGET_DIR}/.uv-sync-stamp")
if(EXISTS "${stamp}")
  file(READ "${stamp}" installed_fingerprint)
  if(installed_fingerprint STREQUAL fingerprint)
    set(skip_sync TRUE)
  endif()
endif()

if(NOT skip_sync)
  file(MAKE_DIRECTORY "${TARGET_DIR}")
  set(requirements "${CMAKE_CURRENT_BINARY_DIR}/uv-${CONFIGURATION}-requirements.txt")
  execute_process(
    COMMAND "${UV_EXECUTABLE}" export --project "${PROJECT_DIR}" --frozen
            --no-dev --no-emit-project --format requirements-txt
            --output-file "${requirements}"
    RESULT_VARIABLE export_result
    OUTPUT_VARIABLE export_stdout ERROR_VARIABLE export_stderr)
  if(NOT export_result EQUAL 0)
    message(FATAL_ERROR "uv export failed (${export_result}):\n${export_stdout}\n${export_stderr}")
  endif()
  execute_process(
    COMMAND "${UV_EXECUTABLE}" pip sync --python "${PYTHON_EXECUTABLE}"
            --target "${TARGET_DIR}" --allow-empty-requirements "${requirements}"
    RESULT_VARIABLE sync_result
    OUTPUT_VARIABLE sync_stdout ERROR_VARIABLE sync_stderr)
  file(REMOVE "${requirements}")
  if(NOT sync_result EQUAL 0)
    message(FATAL_ERROR "uv pip sync failed (${sync_result}):\n${sync_stdout}\n${sync_stderr}")
  endif()
  file(WRITE "${stamp}" "${fingerprint}")
endif()

if(CONFIGURATION STREQUAL "Debug")
  file(GLOB_RECURSE native_extensions
       "${TARGET_DIR}/*.pyd" "${TARGET_DIR}/*.so" "${TARGET_DIR}/*.dll")
  if(native_extensions)
    list(JOIN native_extensions "\n  " native_text)
    message(FATAL_ERROR
      "Debug Python environment contains native extensions. Build Debug-ABI "
      "artifacts explicitly or keep Debug dependencies pure Python:\n  ${native_text}")
  endif()
endif()
