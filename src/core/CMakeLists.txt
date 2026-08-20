# src/core/CMakeLists.txt
# 核心静态库

add_library(caf_plugin_core STATIC
    dynamic_library.cpp
    service_registry.cpp
    checkpoint_manager.cpp
    graceful_shutdown.cpp
    plugin_manager.cpp
    plugin_loader.cpp
)

target_include_directories(caf_plugin_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(caf_plugin_core PUBLIC CAF::core)

if(WIN32)
    target_link_libraries(caf_plugin_core PRIVATE kernel32)
endif()
