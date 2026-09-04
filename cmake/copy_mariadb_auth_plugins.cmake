# libmariadb loads authentication DLLs dynamically, so they are not part of
# the link-time dependency closure. Deploy them beside the executable, where
# the Windows loader finds them without changing the server's authentication.
if(NOT WIN32)
  return()
endif()

foreach(required SOURCE_BIN_DIR DEST_DIR)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "${required} is required for MariaDB authentication deployment")
  endif()
endforeach()

get_filename_component(auth_dir
  "${SOURCE_BIN_DIR}/../plugins/libmariadb" ABSOLUTE)
if(NOT EXISTS "${auth_dir}/caching_sha2_password.dll")
  message(FATAL_ERROR "MySQL 8 authentication plugin is missing: ${auth_dir}/caching_sha2_password.dll")
endif()
file(GLOB auth_plugins "${auth_dir}/*.dll")
file(MAKE_DIRECTORY "${DEST_DIR}")
file(COPY ${auth_plugins} DESTINATION "${DEST_DIR}")
