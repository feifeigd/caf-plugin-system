-- 示例脚本插件：echo
-- 被 lua_host 宿主加载，注册为服务 "echo_service"。
-- 约定：顶层 plugin 表声明清单；on_* 函数实现生命周期与业务。

plugin = {
  name = "echo",
  version = "1.0",
  provides = "echo_service",
  deps = {},   -- 可选依赖的服务名列表（bridge.call 可直接调用）
  protocols = { [1] = "echo" },   -- 外部协议号 1 → 内部 function "echo"
}

local counter = 0

function on_init(manager)
  log("INFO", "echo script initialized, manager=" .. tostring(manager))
end

function on_call(fn, payload)
  counter = counter + 1
  if fn == "echo" then
    return "echo:" .. counter .. ":" .. payload
  end
  return "unknown function=" .. tostring(fn)
end

function on_string(cmd)
  counter = counter + 1
  return "echo-string:" .. counter .. ":" .. cmd
end

function on_save()
  return tostring(counter)
end

function on_restore(state_str)
  counter = tonumber(state_str) or 0
  log("INFO", "echo restored counter=" .. tostring(counter))
end

function on_shutdown()
  log("INFO", "echo script shutting down, counter=" .. tostring(counter))
end
