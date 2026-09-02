# plugins/py_host/CMakeLists.txt
# Python 脚本宿主插件：内嵌 CPython，把 scripts/*.py 包装成 CAF 服务。
# Python 走 vcpkg python3（vcpkg.json 依赖）；find_package(Python3) 由
# vcpkg toolchain 解析到 vcpkg_installed 的 Python3::Python（官方 usage 同款）。

add_library(py_host_plugin SHARED
    py_host_plugin.cpp
)

target_include_directories(py_host_plugin PRIVATE ${Python3_INCLUDE_DIRS})
target_link_libraries(py_host_plugin PRIVATE caf_plugin_core Python3::Python)

# 定义插件名称（供 logging_service.hpp 的 LOG_* 宏 + PLUGIN_CONFIG 配置段使用）
target_compile_definitions(py_host_plugin PRIVATE PLUGIN_NAME="py_host")

set_target_properties(py_host_plugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/py_host"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/py_host"
)

if(WIN32)
    set_target_properties(py_host_plugin PROPERTIES PREFIX "")
else()
    set_target_properties(py_host_plugin PROPERTIES PREFIX "lib")
endif()

apply_plugin_rpath(py_host_plugin)
