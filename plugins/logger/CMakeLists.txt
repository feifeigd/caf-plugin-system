# plugins/logger/CMakeLists.txt
# 构建 Logger 插件（动态库）

# 创建 SHARED 动态库（.dll / .so / .dylib）
add_library(logger_plugin SHARED
    logger_plugin.cpp
)

# 链接父目录已定义的核心库（自动继承 include 路径和 CAF 依赖）
target_link_libraries(logger_plugin PRIVATE caf_plugin_core)

# 统一输出到 build/plugins/logger/
set_target_properties(logger_plugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/logger"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/logger"
)

# Windows: 去掉 lib 前缀，保持 logger_plugin.dll
if(WIN32)
    set_target_properties(logger_plugin PROPERTIES PREFIX "")
else()
    set_target_properties(logger_plugin PROPERTIES PREFIX "lib")
endif()
