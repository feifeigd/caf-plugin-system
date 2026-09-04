add_library(entity_store_plugin SHARED entity_store_plugin.cpp)
target_link_libraries(entity_store_plugin PRIVATE caf_plugin_core)
target_compile_definitions(entity_store_plugin PRIVATE PLUGIN_NAME="entity_store")

set_target_properties(entity_store_plugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/entity_store"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/entity_store"
)

if(WIN32)
    set_target_properties(entity_store_plugin PROPERTIES PREFIX "")
else()
    set_target_properties(entity_store_plugin PROPERTIES PREFIX "lib")
endif()

apply_plugin_rpath(entity_store_plugin)
