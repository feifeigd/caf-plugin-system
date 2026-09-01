// 示例脚本插件：echo（TypeScript）
// 被 ts_host 宿主加载（构建时先 tsc 预编译成 .js），注册为服务 "echo_service"。
// 约定：全局 plugin 对象声明清单；全局 on_* 函数实现生命周期与业务。

// 注意：顶层用 var（而非 const/let）——var 会成为全局对象属性，宿主才能
// 通过 JS_GetPropertyStr(global, "plugin") 读到清单；const/let 只在脚本作用域。
var plugin = {
  name: "echo",
  version: "1.0",
  provides: "echo_service",
  deps: [],
  protocols: { 1: "echo" },   // 外部协议号 1 → 内部 function "echo"
};

let counter = 0;

function on_init(manager: string): void {
  log("INFO", "echo script initialized, manager=" + manager);
}

function on_call(fn: string, payload: string): string {
  counter++;
  if (fn === "echo") {
    return "echo:" + counter + ":" + payload;
  }
  return "unknown function=" + fn;
}

function on_string(cmd: string): string {
  counter++;
  return "echo-string:" + counter + ":" + cmd;
}

function on_save(): string {
  return String(counter);
}

function on_restore(state_str: string): void {
  counter = state_str ? parseInt(state_str, 10) : 0;
  log("INFO", "echo restored counter=" + counter);
}

function on_shutdown(): void {
  log("INFO", "echo script shutting down, counter=" + counter);
}
