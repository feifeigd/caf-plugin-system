find_package(unofficial-sqlite3 CONFIG REQUIRED)

add_library(sqlite_plugin SHARED sqlite_plugin.cpp)
target_link_libraries(sqlite_plugin PRIVATE caf_plugin_core unofficial::sqlite3::sqlite3)
target_compile_definitions(sqlite_plugin PRIVATE PLUGIN_NAME="sqlite")

set_target_properties(sqlite_plugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/sqlite"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/sqlite"
)

if(WIN32)
    set_target_properties(sqlite_plugin PROPERTIES PREFIX "")
else()
    set_target_properties(sqlite_plugin PROPERTIES PREFIX "lib")
endif()

apply_plugin_rpath(sqlite_plugin)
