# plugins/redis/CMakeLists.txt
# Redis 插件（数据库插件系列参考实现）

find_package(hiredis CONFIG REQUIRED)

add_library(redis_plugin SHARED
    redis_plugin.cpp
)

target_link_libraries(redis_plugin PRIVATE caf_plugin_core hiredis::hiredis)

# 定义插件名称（供 logging_service.hpp 的 LOG_* 宏使用）
target_compile_definitions(redis_plugin PRIVATE PLUGIN_NAME="redis")

set_target_properties(redis_plugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/redis"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/redis"
)

if(WIN32)
    set_target_properties(redis_plugin PROPERTIES PREFIX "")
else()
    set_target_properties(redis_plugin PROPERTIES PREFIX "lib")
endif()

apply_plugin_rpath(redis_plugin)
