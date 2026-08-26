# plugins/postgres/CMakeLists.txt
# PostgreSQL 插件（libpq）

find_package(PostgreSQL REQUIRED)

add_library(postgres_plugin SHARED
    postgres_plugin.cpp
)

target_link_libraries(postgres_plugin PRIVATE caf_plugin_core PostgreSQL::PostgreSQL)

target_compile_definitions(postgres_plugin PRIVATE PLUGIN_NAME="postgres")

set_target_properties(postgres_plugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/postgres"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/postgres"
)

if(WIN32)
    set_target_properties(postgres_plugin PROPERTIES PREFIX "")
else()
    set_target_properties(postgres_plugin PROPERTIES PREFIX "lib")
endif()

apply_plugin_rpath(postgres_plugin)
