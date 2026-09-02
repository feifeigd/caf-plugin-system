foreach(required_var IN ITEMS SOURCE_DIR DEST_DIR STAMP_FILE PYTHON_VERSION CONFIGURATION)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "sync_python_stdlib: ${required_var} is required")
    endif()
endforeach()

set(sentinel_files
    "${SOURCE_DIR}/os.py"
    "${SOURCE_DIR}/encodings/__init__.py")
foreach(sentinel IN LISTS sentinel_files)
    if(NOT EXISTS "${sentinel}")
        message(FATAL_ERROR "Python standard-library sentinel missing: ${sentinel}")
    endif()
endforeach()

file(SHA256 "${SOURCE_DIR}/os.py" os_hash)
file(SHA256 "${SOURCE_DIR}/encodings/__init__.py" encodings_hash)
string(SHA256 source_fingerprint
    "${SOURCE_DIR}|${PYTHON_VERSION}|${CONFIGURATION}|${os_hash}|${encodings_hash}")

set(previous_fingerprint "")
if(EXISTS "${STAMP_FILE}")
    file(READ "${STAMP_FILE}" previous_fingerprint)
    string(STRIP "${previous_fingerprint}" previous_fingerprint)
endif()

if(previous_fingerprint STREQUAL source_fingerprint
   AND EXISTS "${DEST_DIR}/os.py"
   AND EXISTS "${DEST_DIR}/encodings/__init__.py")
    message(STATUS "Python ${PYTHON_VERSION} standard library is current (${CONFIGURATION})")
    return()
endif()

file(MAKE_DIRECTORY "${DEST_DIR}")
file(COPY "${SOURCE_DIR}/" DESTINATION "${DEST_DIR}")
get_filename_component(stamp_dir "${STAMP_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${stamp_dir}")
file(WRITE "${STAMP_FILE}" "${source_fingerprint}\n")
message(STATUS "Staged Python ${PYTHON_VERSION} standard library (${CONFIGURATION})")
