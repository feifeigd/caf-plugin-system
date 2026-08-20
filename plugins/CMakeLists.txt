# plugins/CMakeLists.txt
# 插件总入口：依次加载各插件子目录

add_subdirectory(logger)
add_subdirectory(business)
add_subdirectory(platform)
