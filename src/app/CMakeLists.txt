# src/app/CMakeLists.txt
# 主程序可执行文件

add_executable(caf_plugin_app
    main.cpp
    test/app_backdoors.cpp
    test/app_tests_fixed.cpp
    ../core/plugin/dynamic_library.cpp
)

# 回退 938c621 延迟加载架构（2026-08-31 用户拍板）：exe 静态导入 core
target_link_libraries(caf_plugin_app PRIVATE caf_plugin_core)

# 测试代码（--test-* 后门）编译进 exe：需要 core 私有头 + 公共头。
# 改测试只重编本 exe，core.dll / 插件 DLL 均不动（避免全链重建）。
target_include_directories(caf_plugin_app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/test
    ${CMAKE_CURRENT_SOURCE_DIR}/../core
    ${CMAKE_SOURCE_DIR}/include
)

if(UNIX AND NOT APPLE)
    target_link_libraries(caf_plugin_app PRIVATE dl)
endif()

# Linux 部署：第三方依赖 .so 分类放 exe_dir/lib/，exe 的 RPATH 指向它
#（$ORIGIN 运行时按 exe 所在目录解析；拷贝部署不依赖 install 流程）
if(UNIX)
    set_target_properties(caf_plugin_app PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN/lib")
endif()
