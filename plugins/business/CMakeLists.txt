cmake_minimum_required(VERSION 3.20)
project(business_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(CAF REQUIRED COMPONENTS core)

add_library(business_plugin SHARED business_plugin.cpp)
target_include_directories(business_plugin PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
target_link_libraries(business_plugin PRIVATE CAF::core)

if(WIN32)
  set_target_properties(business_plugin PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_NAME "business_plugin"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/business"
  )
else()
  set_target_properties(business_plugin PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "business_plugin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/business"
  )
endif()
