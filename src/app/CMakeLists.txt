# src/app/CMakeLists.txt
# 主程序可执行文件

add_executable(caf_plugin_app
    main.cpp
    app_tests.cpp
    app_backdoors.cpp
)

target_link_libraries(caf_plugin_app PRIVATE caf_plugin_core)

# TODO(leakfix): exe 不静态导入 caf_plugin_core.dll——延迟加载，退出前手动
# FreeLibrary 卸载 → 强制 static 析构（leak dump 前释放进程级持有对象）
if(WIN32)
    target_link_libraries(caf_plugin_app PRIVATE delayimp)
    target_link_options(caf_plugin_app PRIVATE
        "/DELAYLOAD:caf_plugin_core.dll" "/DELAYLOAD:caf_core.dll")
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(caf_plugin_app PRIVATE dl)
endif()

# Linux 部署：第三方依赖 .so 分类放 exe_dir/lib/，exe 的 RPATH 指向它
#（$ORIGIN 运行时按 exe 所在目录解析；拷贝部署不依赖 install 流程）
if(UNIX)
    set_target_properties(caf_plugin_app PROPERTIES
        INSTALL_RPATH "$ORIGIN/lib")
endif()
