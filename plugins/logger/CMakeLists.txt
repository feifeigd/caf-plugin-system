cmake_minimum_required(VERSION 3.20)
project(logger_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(CAF REQUIRED COMPONENTS core)

add_library(logger_plugin SHARED logger_plugin.cpp)
target_include_directories(logger_plugin PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
target_link_libraries(logger_plugin PRIVATE CAF::core)

if(WIN32)
  set_target_properties(logger_plugin PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_NAME "logger_plugin"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/logger"
  )
else()
  set_target_properties(logger_plugin PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "logger_plugin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/logger"
  )
endif()
