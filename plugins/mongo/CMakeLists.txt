# ------------------------------------------------------------------
# MongoDB 插件（mongo-cxx-driver）
# vcpkg mongo-cxx-driver 提供官方 CMake config（find_package(mongocxx)）。
# target 名随 triplet 变化：Windows 动态构建为 mongo::mongocxx_shared，
# Linux x64-linux 静态构建为 mongo::mongocxx_static，这里做条件选择。
# ------------------------------------------------------------------

find_package(mongocxx CONFIG REQUIRED)

add_library(mongo_plugin SHARED
    mongo_plugin.cpp
)

if(TARGET mongo::mongocxx_shared)
    set(MONGOCXX_LIB mongo::mongocxx_shared)
else()
    set(MONGOCXX_LIB mongo::mongocxx_static)
endif()
target_link_libraries(mongo_plugin PRIVATE caf_plugin_core ${MONGOCXX_LIB})

target_compile_definitions(mongo_plugin PRIVATE PLUGIN_NAME="mongo")

set_target_properties(mongo_plugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/mongo"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/mongo"
)

if(WIN32)
    set_target_properties(mongo_plugin PROPERTIES PREFIX "")
else()
    set_target_properties(mongo_plugin PROPERTIES PREFIX "lib")
endif()

apply_plugin_rpath(mongo_plugin)
