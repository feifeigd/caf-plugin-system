# ------------------------------------------------------------------
# MongoDB 插件（mongo-cxx-driver）
# vcpkg mongo-cxx-driver 提供官方 CMake config（find_package(mongocxx)），
# target 名 mongo::mongocxx_shared（含 mongo::bsoncxx_shared）。
# ------------------------------------------------------------------

find_package(mongocxx CONFIG REQUIRED)

add_library(mongo_plugin SHARED
    mongo_plugin.cpp
)

target_link_libraries(mongo_plugin PRIVATE caf_plugin_core mongo::mongocxx_shared)

target_compile_definitions(mongo_plugin PRIVATE PLUGIN_NAME="mongo")

# mongocxx 的 C 驱动头依赖 winsock2，编译期需要 windows 相关宏（DRIVER_WIN32 由库自带）
