# src/app/CMakeLists.txt
# 主程序可执行文件

add_executable(caf_plugin_app
    main.cpp
    app_tests.cpp
    app_backdoors.cpp
)

target_link_libraries(caf_plugin_app PRIVATE caf_plugin_core)

if(UNIX AND NOT APPLE)
    target_link_libraries(caf_plugin_app PRIVATE dl)
endif()

# Linux 部署：第三方依赖 .so 分类放 exe_dir/lib/，exe 的 RPATH 指向它
#（$ORIGIN 运行时按 exe 所在目录解析；拷贝部署不依赖 install 流程）
if(UNIX)
    set_target_properties(caf_plugin_app PROPERTIES
        INSTALL_RPATH "$ORIGIN/lib")
endif()
