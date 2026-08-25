# plugins/mysql/CMakeLists.txt
# MySQL 插件（libmariadb）
# vcpkg 的 libmariadb 不提供官方 CMake config，用 unofficial 包
#（target 名 unofficial::libmariadb，头文件在 include/mysql/）

find_package(unofficial-libmariadb CONFIG REQUIRED)

add_library(mysql_plugin SHARED
    mysql_plugin.cpp
)

target_link_libraries(mysql_plugin PRIVATE caf_plugin_core unofficial::libmariadb)

target_compile_definitions(mysql_plugin PRIVATE PLUGIN_NAME="mysql")

set_target_properties(mysql_plugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/mysql"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/mysql"
)

if(WIN32)
    set_target_properties(mysql_plugin PROPERTIES PREFIX "")
else()
    set_target_properties(mysql_plugin PROPERTIES PREFIX "lib")
endif()

apply_plugin_rpath(mysql_plugin)
