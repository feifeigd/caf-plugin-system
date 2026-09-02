if(NOT DEFINED SOURCE_FILE OR NOT EXISTS "${SOURCE_FILE}")
  message(FATAL_ERROR "SOURCE_FILE not found: ${SOURCE_FILE}")
endif()
if(NOT DEFINED DEST_FILE)
  message(FATAL_ERROR "DEST_FILE is required")
endif()

# config = 文件的全部内容
file(READ "${SOURCE_FILE}" config)

# 多行原始字符串
# 这里是替换 caf-application.conf 中配置文件的内容
set(shared_config [=[
  # staged runtime: script sources are configuration-independent and shared
  lua_host { scripts_dir = "../shared/scripts/lua" }
  py_host  { scripts_dir = "../shared/scripts/python" }
  ts_host  { scripts_dir = "../shared/scripts/typescript" }
]=])
string(REPLACE "  # @STAGED_SHARED_SCRIPT_DIRS@" "${shared_config}"
               config "${config}")
file(WRITE "${DEST_FILE}" "${config}")
