# 示例脚本插件：echo
# 被 py_host 宿主加载，注册为服务 "echo_service"。
# 约定：顶层 plugin dict 声明清单；on_* 函数实现生命周期与业务。

plugin = {
    "name": "echo",
    "version": "1.0",
    "provides": "echo_service",
    "deps": [],   # 可选依赖的服务名列表（bridge.call 可直接调用）
}

counter = 0


def on_init(manager):
    log("INFO", "echo script initialized, manager=" + str(manager))


def on_call(sub_proto, payload):
    global counter
    counter = counter + 1
    if sub_proto == 1:
        return "echo:" + str(counter) + ":" + payload
    return "unknown sub_proto=" + str(sub_proto)


def on_string(cmd):
    global counter
    counter = counter + 1
    return "echo-string:" + str(counter) + ":" + cmd


def on_save():
    return str(counter)


def on_restore(state_str):
    global counter
    counter = int(state_str) if state_str else 0
    log("INFO", "echo restored counter=" + str(counter))


def on_shutdown():
    log("INFO", "echo script shutting down, counter=" + str(counter))
