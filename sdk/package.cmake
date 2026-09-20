# caf-plugin-sdk 打包脚本
#
# 用法（仓库根或任意目录）：
#   cmake -P sdk/package.cmake
# 或双击/执行 sdk/package.bat
#
# 从最新构建树组装插件开发 SDK 到 <仓库根>/caf-plugin-sdk/：
#   1. 拷贝 sdk/template/ 的静态文件（README/vcpkg.json/cmake 配置/示例）
#   2. 从 include/ 与 src/core/ 拷贝插件所需公共头文件（保持原始相对布局）
#   3. 从 docs/ 拷贝插件开发指南
#   4. 从 out/build/<预设>/src/core/<Config>/ 拷贝 caf_plugin_core（dll+lib），
#      Debug/Release 各自独立，存在哪个拷哪个
#   5. 写 VERSION.txt（git 短哈希 + 时间），便于核对 SDK 与框架版本一致
#
# 注意：本脚本只拷贝、不构建。先构建 core（cmake --build ... --config Debug）。

cmake_minimum_required(VERSION 3.20)

get_filename_component(SDK_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(TEMPLATE_DIR "${CMAKE_CURRENT_LIST_DIR}/template")
set(OUT_DIR "${SDK_SRC_DIR}/caf-plugin-sdk")

# ---- 构建树定位：环境变量优先，其次默认预设目录 ----
if(DEFINED ENV{CAF_BUILD_DIR})
    set(BUILD_DIR "$ENV{CAF_BUILD_DIR}")
else()
    set(BUILD_DIR "${SDK_SRC_DIR}/out/build/windows-x64")
endif()

if(NOT EXISTS "${BUILD_DIR}/src/core")
    message(FATAL_ERROR
        "未找到构建树：${BUILD_DIR}\n"
        "请先构建 caf_plugin_core，或用环境变量 CAF_BUILD_DIR 指定构建目录。")
endif()

message(STATUS "SDK 输出目录: ${OUT_DIR}")
message(STATUS "构建树:       ${BUILD_DIR}")

# ---- 1. 模板静态文件 ----
file(COPY "${TEMPLATE_DIR}/README.md"
          "${TEMPLATE_DIR}/vcpkg.json"
     DESTINATION "${OUT_DIR}")
file(COPY "${TEMPLATE_DIR}/cmake"     DESTINATION "${OUT_DIR}")
file(COPY "${TEMPLATE_DIR}/examples"  DESTINATION "${OUT_DIR}")

# ---- 2. 公共头文件（插件编译所需的最小集合，保持框架内相对布局）----
file(MAKE_DIRECTORY
    "${OUT_DIR}/include/common"
    "${OUT_DIR}/include/services"
    "${OUT_DIR}/include/plugin")

file(GLOB COMMON_HEADERS "${SDK_SRC_DIR}/include/common/*.hpp"
                         "${SDK_SRC_DIR}/include/common/*.def")
file(COPY ${COMMON_HEADERS}             DESTINATION "${OUT_DIR}/include/common")
file(COPY "${SDK_SRC_DIR}/include/services/logging_service.hpp"
                                        DESTINATION "${OUT_DIR}/include/services")
file(COPY "${SDK_SRC_DIR}/src/core/plugin/plugin_interface.hpp"
          "${SDK_SRC_DIR}/src/core/plugin/plugin_lifecycle.hpp"
                                        DESTINATION "${OUT_DIR}/include/plugin")
file(COPY "${SDK_SRC_DIR}/src/core/graceful_shutdown.hpp"
                                        DESTINATION "${OUT_DIR}/include")

# ---- 3. 插件开发指南 ----
file(MAKE_DIRECTORY "${OUT_DIR}/docs")
file(COPY "${SDK_SRC_DIR}/docs/plugin-guide.md" DESTINATION "${OUT_DIR}/docs")

# ---- 4. caf_plugin_core 二进制（存在哪个配置拷哪个）----
set(COPIED_CONFIGS "")
foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
    set(core_dir "${BUILD_DIR}/src/core/${cfg}")
    if(EXISTS "${core_dir}/caf_plugin_core.dll" OR EXISTS "${core_dir}/caf_plugin_core.lib")
        file(MAKE_DIRECTORY "${OUT_DIR}/lib/${cfg}")
        foreach(ext dll lib)
            if(EXISTS "${core_dir}/caf_plugin_core.${ext}")
                file(COPY "${core_dir}/caf_plugin_core.${ext}"
                     DESTINATION "${OUT_DIR}/lib/${cfg}")
            endif()
        endforeach()
        list(APPEND COPIED_CONFIGS "${cfg}")
    endif()
endforeach()

if(NOT COPIED_CONFIGS)
    message(FATAL_ERROR
        "${BUILD_DIR}/src/core/<Config>/ 下没有任何 caf_plugin_core 产物，"
        "请先构建 core 再打包。")
endif()

# ---- 5. 版本戳 ----
set(GIT_HASH "unknown")
find_program(GIT_EXE NAMES git)
if(GIT_EXE)
    execute_process(
        COMMAND "${GIT_EXE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${SDK_SRC_DIR}"
        OUTPUT_VARIABLE GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
string(TIMESTAMP STAMP "%Y-%m-%d %H:%M:%S")
file(WRITE "${OUT_DIR}/VERSION.txt"
     "caf-plugin-sdk generated ${STAMP}\nframework git: ${GIT_HASH}\nconfigs: ${COPIED_CONFIGS}\n")

message(STATUS "已拷贝配置: ${COPIED_CONFIGS}")
message(STATUS "完成。SDK 位于 ${OUT_DIR}")
