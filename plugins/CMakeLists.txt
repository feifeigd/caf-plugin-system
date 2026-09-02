# plugins/CMakeLists.txt
# 插件总入口：依次加载各插件子目录

# Linux 部署公共 RPATH：插件 .so 在 plugins/<name>/，第三方依赖 .so 分类放
# exe_dir/lib/ —— dlopen 插件时其依赖（DT_NEEDED）按 $ORIGIN/../lib 解析。
# Windows 侧由 framework_bootstrap 的 AddDllDirectory(exe_dir/lib) 等效实现。
function(apply_plugin_rpath target)
    if(UNIX)
        set_target_properties(${target} PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "$ORIGIN/../../lib")
    endif()
endfunction()

add_subdirectory(business)
add_subdirectory(platform)
add_subdirectory(cluster_admin)
add_subdirectory(redis)
add_subdirectory(mysql)
add_subdirectory(postgres)
add_subdirectory(sqlite)
add_subdirectory(mongo)
add_subdirectory(lua_host)
add_subdirectory(py_host)
add_subdirectory(ts_host)
