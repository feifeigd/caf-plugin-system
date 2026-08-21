# src/app/CMakeLists.txt
# 主程序可执行文件

add_executable(caf_plugin_app
    main.cpp
)

target_link_libraries(caf_plugin_app PRIVATE caf_plugin_core caf_plugin_cluster)

if(UNIX AND NOT APPLE)
    target_link_libraries(caf_plugin_app PRIVATE dl)
endif()
