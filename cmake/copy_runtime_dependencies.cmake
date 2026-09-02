foreach(required BINARIES SEARCH_DIR ALLOWED_ROOT DEST_DIR PLATFORM_KIND)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

string(REPLACE "|" ";" binaries "${BINARIES}")
foreach(binary IN LISTS binaries)
  if(NOT EXISTS "${binary}")
    message(FATAL_ERROR "Runtime dependency input not found: ${binary}")
  endif()
endforeach()

list(POP_FRONT binaries app_executable)
file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES "${app_executable}"
  LIBRARIES ${binaries}
  DIRECTORIES "${SEARCH_DIR}"
  RESOLVED_DEPENDENCIES_VAR resolved
  UNRESOLVED_DEPENDENCIES_VAR unresolved
  CONFLICTING_DEPENDENCIES_PREFIX conflicts
  PRE_EXCLUDE_REGEXES "api-ms-win-.*" "ext-ms-win-.*"
  POST_EXCLUDE_REGEXES ".*[Ww]indows[/\\\\][Ss]ystem32.*" "^/lib/.*" "^/usr/lib/.*")

# Visual Studio may place identical transitive DLLs beside every target. Resolve
# those duplicate names from the selected vcpkg Debug/Release runtime directory.
foreach(conflict_name IN LISTS conflicts_FILENAMES)
  if(EXISTS "${SEARCH_DIR}/${conflict_name}")
    list(APPEND resolved "${SEARCH_DIR}/${conflict_name}")
  else()
    message(FATAL_ERROR
      "Conflicting dependency '${conflict_name}' has no canonical copy in ${SEARCH_DIR}")
  endif()
endforeach()

file(REAL_PATH "${ALLOWED_ROOT}" allowed_root)
file(MAKE_DIRECTORY "${DEST_DIR}")
set(files_to_copy)
foreach(dependency IN LISTS resolved)
  file(REAL_PATH "${dependency}" dependency_real)
  string(FIND "${dependency_real}" "${allowed_root}/" root_pos)
  if(root_pos EQUAL 0)
    list(APPEND files_to_copy "${dependency}")
  else()
    get_filename_component(dependency_name "${dependency}" NAME)
    if(EXISTS "${SEARCH_DIR}/${dependency_name}")
      list(APPEND files_to_copy "${SEARCH_DIR}/${dependency_name}")
    endif()
  endif()
endforeach()
list(REMOVE_DUPLICATES files_to_copy)

set(manifest "${DEST_DIR}/.vcpkg-runtime-manifest")
if(EXISTS "${manifest}")
  file(STRINGS "${manifest}" previous_names)
  foreach(previous_name IN LISTS previous_names)
    if(NOT previous_name STREQUAL "")
      file(REMOVE "${DEST_DIR}/${previous_name}")
    endif()
  endforeach()
endif()

set(deployed_names)
foreach(dependency IN LISTS files_to_copy)
  file(COPY "${dependency}" DESTINATION "${DEST_DIR}" FOLLOW_SYMLINK_CHAIN)
  get_filename_component(dependency_name "${dependency}" NAME)
  list(APPEND deployed_names "${dependency_name}")
endforeach()

# The pre-manifest Windows layout copied every DLL. Remove vcpkg DLLs that are
# not in the resolved closure so the first migration is deterministic as well.
if(PLATFORM_KIND STREQUAL "WINDOWS")
  file(GLOB available_runtime_files "${SEARCH_DIR}/*.dll")
  foreach(available IN LISTS available_runtime_files)
    get_filename_component(available_name "${available}" NAME)
    if(NOT available_name IN_LIST deployed_names)
      file(REMOVE "${DEST_DIR}/${available_name}")
    endif()
  endforeach()
endif()

list(REMOVE_DUPLICATES deployed_names)
list(JOIN deployed_names "\n" manifest_text)
file(WRITE "${manifest}" "${manifest_text}\n")

list(REMOVE_ITEM unresolved "caf_plugin_core.dll" "libcaf_plugin_core.so")
if(unresolved)
  list(JOIN unresolved ", " unresolved_text)
  message(WARNING "Unresolved runtime dependencies: ${unresolved_text}")
endif()
list(LENGTH files_to_copy copied)
message(STATUS "Copied ${copied} resolved vcpkg runtime dependencies")
