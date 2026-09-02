cmake_minimum_required(VERSION 3.20)

# ------------------------------------------------------------------
# vcpkg toolchain 必须在 project() 之前设置
# 优先级：1) 环境变量 2) 项目同级目录 3) VS2022 自带
# ------------------------------------------------------------------
set(VCPKG_TOOLCHAIN_CANDIDATES
    "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg/scripts/buildsystems/vcpkg.cmake"
    "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/vcpkg/scripts/buildsystems/vcpkg.cmake"
)

foreach(candidate IN LISTS VCPKG_TOOLCHAIN_CANDIDATES)
    if(EXISTS "${candidate}")
        set(CMAKE_TOOLCHAIN_FILE "${candidate}" CACHE STRING "")
        message(STATUS "vcpkg toolchain: ${candidate}")
        break()
    endif()
endforeach()

project(caf-plugin-system VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# UTF-8 源码（中文注释/字符串）在 MSVC 默认 936 代码页下解析会错乱
# （C4819 + 后续 C2065 误报）。/utf-8 让 MSVC 按 UTF-8 解析源码。
if(MSVC)
    add_compile_options(/utf-8)
endif()

# 如果没找到 vcpkg，给出清晰的提示
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE OR NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    message(FATAL_ERROR
        "vcpkg toolchain not found.\n"
        "Please install vcpkg and set VCPKG_ROOT environment variable,\n"
        "or place vcpkg in this project's root directory.\n"
        "See: https://github.com/microsoft/vcpkg#quick-start")
endif()

find_package(CAF REQUIRED COMPONENTS core)
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)

# CAF 内部 Debug 日志只在 Debug 配置编译。Release/RelWithDebInfo 不定义
# CAF_LOG_LEVEL，避免把框架调度级日志字符串与分支带进生产二进制。
add_compile_definitions(
    $<$<CONFIG:Debug>:CAF_LOG_LEVEL=CAF_LOG_LEVEL_DEBUG>
)

# Core library + Main executable
add_subdirectory(src)

# Plugins
add_subdirectory(plugins)

# 跨平台运行目录自动组装。Debug/Release 严格隔离；脚本资源共享。
# Windows DLL 与 EXE 同目录；Linux ELF 依赖集中在 lib/。
if(WIN32 OR UNIX)
    set(APP_RUN_ROOT "${CMAKE_SOURCE_DIR}/run")
    set(APP_RUN_DIR "${APP_RUN_ROOT}/$<CONFIG>")
    if(WIN32)
        set(APP_NATIVE_LIB_DIR "${APP_RUN_DIR}")
        set(RUNTIME_PLATFORM_KIND "WINDOWS")
        set(PYTHON_STDLIB_DEST "${APP_RUN_DIR}/Lib")
        set(VCPKG_RUNTIME_DIR
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<IF:$<CONFIG:Debug>,debug/bin,bin>")
    else()
        set(APP_NATIVE_LIB_DIR "${APP_RUN_DIR}/lib")
        set(RUNTIME_PLATFORM_KIND "UNIX")
        set(PYTHON_STDLIB_DEST
            "${APP_RUN_DIR}/lib/python${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}")
        set(VCPKG_RUNTIME_DIR
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<IF:$<CONFIG:Debug>,debug/lib,lib>")
    endif()
    find_program(UV_EXECUTABLE NAMES uv REQUIRED)
    file(TO_CMAKE_PATH "${Python3_STDLIB}" PYTHON_STDLIB_SOURCE)
    set(PYTHON_STDLIB_STAMP
        "${CMAKE_BINARY_DIR}/stage/$<CONFIG>/python-stdlib.stamp")
    add_custom_target(stage_python_stdlib
        COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE_DIR=${PYTHON_STDLIB_SOURCE}"
                "-DDEST_DIR=${PYTHON_STDLIB_DEST}"
                "-DSTAMP_FILE=${PYTHON_STDLIB_STAMP}"
                "-DPYTHON_VERSION=${Python3_VERSION}"
                "-DCONFIGURATION=$<CONFIG>"
                -P "${CMAKE_SOURCE_DIR}/cmake/sync_python_stdlib.cmake"
        COMMENT "Staging Python ${Python3_VERSION} standard library ($<CONFIG>)"
        VERBATIM
    )

    add_custom_target(sync_python_deps
        COMMAND "${CMAKE_COMMAND}"
                "-DUV_EXECUTABLE=${UV_EXECUTABLE}"
                "-DPYTHON_EXECUTABLE=${Python3_EXECUTABLE}"
                "-DPROJECT_DIR=${CMAKE_SOURCE_DIR}/plugins/py_host/python"
                "-DTARGET_DIR=${PYTHON_STDLIB_DEST}/site-packages"
                "-DCONFIGURATION=$<CONFIG>"
                "-DPLATFORM_KIND=${RUNTIME_PLATFORM_KIND}"
                -P "${CMAKE_SOURCE_DIR}/cmake/sync_python_deps.cmake"
        DEPENDS
            "${CMAKE_SOURCE_DIR}/plugins/py_host/python/pyproject.toml"
            "${CMAKE_SOURCE_DIR}/plugins/py_host/python/uv.lock"
            stage_python_stdlib
        COMMENT "Synchronizing locked Python dependencies with uv ($<CONFIG>)"
        VERBATIM
    )

    set(STAGED_PLUGIN_TARGETS
        business_plugin
        cluster_admin_plugin
        lua_host_plugin
        mongo_plugin
        mysql_plugin
        platform_plugin
        postgres_plugin
        py_host_plugin
        redis_plugin
        ts_host_plugin
    )
    set(RUNTIME_SCAN_BINARIES
        "$<TARGET_FILE:caf_plugin_app>|$<TARGET_FILE:caf_plugin_core>")
    foreach(runtime_target IN LISTS STAGED_PLUGIN_TARGETS)
        string(APPEND RUNTIME_SCAN_BINARIES "|$<TARGET_FILE:${runtime_target}>")
    endforeach()

    add_custom_target(stage_run ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${APP_RUN_DIR}" "${APP_RUN_DIR}/updates"
                "${APP_NATIVE_LIB_DIR}"
                "${APP_RUN_ROOT}/shared/scripts/lua"
                "${APP_RUN_ROOT}/shared/scripts/python"
                "${APP_RUN_ROOT}/shared/scripts/typescript"
        COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE_FILE=${CMAKE_SOURCE_DIR}/caf-application.conf"
                "-DDEST_FILE=${APP_RUN_DIR}/caf-application.conf"
                -P "${CMAKE_SOURCE_DIR}/cmake/write_runtime_config.cmake"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_SOURCE_DIR}/examples/cluster/master.conf"
                "${APP_RUN_DIR}/master.conf"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_SOURCE_DIR}/examples/cluster/worker-a.conf"
                "${APP_RUN_DIR}/worker-a.conf"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_SOURCE_DIR}/examples/cluster/worker-b.conf"
                "${APP_RUN_DIR}/worker-b.conf"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:caf_plugin_app>" "${APP_RUN_DIR}/"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:caf_plugin_core>" "${APP_NATIVE_LIB_DIR}/"
        # 从应用、核心及全部插件计算真实运行时依赖闭包。
        COMMAND "${CMAKE_COMMAND}"
                "-DBINARIES=${RUNTIME_SCAN_BINARIES}"
                "-DSEARCH_DIR=${VCPKG_RUNTIME_DIR}"
                "-DALLOWED_ROOT=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}"
                "-DDEST_DIR=${APP_NATIVE_LIB_DIR}"
                "-DPLATFORM_KIND=${RUNTIME_PLATFORM_KIND}"
                -P "${CMAKE_SOURCE_DIR}/cmake/copy_runtime_dependencies.cmake"

        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${APP_RUN_DIR}/plugins/business"
                "${APP_RUN_DIR}/plugins/cluster_admin"
                "${APP_RUN_DIR}/plugins/lua_host"
                "${APP_RUN_DIR}/plugins/mongo"
                "${APP_RUN_DIR}/plugins/mysql"
                "${APP_RUN_DIR}/plugins/platform"
                "${APP_RUN_DIR}/plugins/postgres"
                "${APP_RUN_DIR}/plugins/py_host"
                "${APP_RUN_DIR}/plugins/redis"
                "${APP_RUN_DIR}/plugins/ts_host"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:business_plugin>" "${APP_RUN_DIR}/plugins/business/"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:cluster_admin_plugin>" "${APP_RUN_DIR}/plugins/cluster_admin/"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:lua_host_plugin>" "${APP_RUN_DIR}/plugins/lua_host/"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:mongo_plugin>" "${APP_RUN_DIR}/plugins/mongo/"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:mysql_plugin>" "${APP_RUN_DIR}/plugins/mysql/"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:platform_plugin>" "${APP_RUN_DIR}/plugins/platform/"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:postgres_plugin>" "${APP_RUN_DIR}/plugins/postgres/"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:py_host_plugin>" "${APP_RUN_DIR}/plugins/py_host/"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:redis_plugin>" "${APP_RUN_DIR}/plugins/redis/"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:ts_host_plugin>" "${APP_RUN_DIR}/plugins/ts_host/"

        # 清理旧布局遗留的按配置脚本副本；脚本只保留 shared/ 一份。
        COMMAND "${CMAKE_COMMAND}" -E rm -rf
                "${APP_RUN_DIR}/plugins/lua_host/scripts"
                "${APP_RUN_DIR}/plugins/py_host/scripts"
                "${APP_RUN_DIR}/plugins/ts_host/scripts"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:business_plugin_v2>"
                "${APP_RUN_DIR}/updates/"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_SOURCE_DIR}/plugins/lua_host/scripts"
                "${APP_RUN_ROOT}/shared/scripts/lua"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_SOURCE_DIR}/plugins/py_host/scripts"
                "${APP_RUN_ROOT}/shared/scripts/python"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_SOURCE_DIR}/plugins/ts_host/scripts"
                "${APP_RUN_ROOT}/shared/scripts/typescript"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_SOURCE_DIR}/plugins/postgres/resource.json"
                "${APP_RUN_DIR}/plugins/postgres/resource.json"
        DEPENDS
            caf_plugin_app
            caf_plugin_core
            business_plugin_v2
            ${STAGED_PLUGIN_TARGETS}
            stage_python_stdlib
            sync_python_deps
        COMMENT "Staging current $<CONFIG> runtime into ${APP_RUN_DIR}"
        VERBATIM
    )
endif()

# Tests
enable_testing()
add_subdirectory(tests)
