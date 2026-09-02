if(NOT DEFINED SOURCE_DIR OR NOT IS_DIRECTORY "${SOURCE_DIR}")
  message(FATAL_ERROR "Runtime library directory not found: ${SOURCE_DIR}")
endif()
if(NOT DEFINED DEST_DIR OR NOT DEFINED PLATFORM_KIND)
  message(FATAL_ERROR "DEST_DIR and PLATFORM_KIND are required")
endif()

file(MAKE_DIRECTORY "${DEST_DIR}")
if(PLATFORM_KIND STREQUAL "WINDOWS")
  file(GLOB runtime_libraries "${SOURCE_DIR}/*.dll")
elseif(PLATFORM_KIND STREQUAL "UNIX")
  file(GLOB runtime_libraries
       "${SOURCE_DIR}/*.so" "${SOURCE_DIR}/*.so.*")
else()
  message(FATAL_ERROR "Unknown PLATFORM_KIND: ${PLATFORM_KIND}")
endif()

if(NOT runtime_libraries)
  message(FATAL_ERROR "No runtime libraries found in: ${SOURCE_DIR}")
endif()
file(COPY ${runtime_libraries} DESTINATION "${DEST_DIR}"
     FOLLOW_SYMLINK_CHAIN)
