cmake_minimum_required(VERSION 3.20)
project(caf-plugin-system VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# vcpkg integration (manifest mode)
if(DEFINED ENV{VCPKG_ROOT})
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

find_package(CAF REQUIRED COMPONENTS core)

# Core library
add_library(caf_plugin_core STATIC
  src/core/dynamic_library.cpp
  src/core/service_registry.cpp
  src/core/checkpoint_manager.cpp
  src/core/graceful_shutdown.cpp
  src/core/plugin_manager.cpp
)

target_include_directories(caf_plugin_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(caf_plugin_core PUBLIC CAF::core)

if(WIN32)
  target_link_libraries(caf_plugin_core PRIVATE kernel32)
endif()

# Main executable
add_executable(caf_plugin_app src/app/main.cpp)
target_link_libraries(caf_plugin_app PRIVATE caf_plugin_core)

if(UNIX AND NOT APPLE)
  target_link_libraries(caf_plugin_app PRIVATE dl)
endif()

# Plugins
add_subdirectory(plugins/logger)
add_subdirectory(plugins/business)

# Tests
enable_testing()
add_executable(test_dependency_graph tests/test_dependency_graph.cpp)
target_include_directories(test_dependency_graph PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
add_test(NAME dependency_graph COMMAND test_dependency_graph)
