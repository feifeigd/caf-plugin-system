// ------------------------------------------------------------------
// 统一时间源实现（time_service）：g_time_offset 的唯一实体。
// 定义在 caf_plugin_core 动态库内部且**不导出**（用户拍板，2026-08-27）：
//   - 数据封装在 DLL 内，规避 Windows 导出数据的 thunk / 跨编译器
//     ABI / 初始化顺序坑；
//   - 所有跨模块访问经下方 CORE_API 导出函数——exe 与各插件 DLL
//     链接同一动态库 → 进程内只有一份实体，设置一次全局生效。
// ------------------------------------------------------------------
#include "services/time_service.hpp"

#include <atomic>
#include <cstring>
#include <ctime>

namespace caf_plugin_system {

namespace {

// 全局业务时间偏移（秒）。DLL 内私有实体，不进导出表。
// relaxed 足够——只要求"最终可见"，不要求跨线程顺序（读偏移本身无
// 依赖序）。启动早期由 framework_bootstrap 注入。
std::atomic<std::chrono::seconds> g_time_offset{std::chrono::seconds{0}};

} // namespace

void set_time_offset(std::chrono::seconds off) noexcept {
    g_time_offset.store(off, std::memory_order_relaxed);
}

std::chrono::seconds time_offset() noexcept {
    return g_time_offset.load(std::memory_order_relaxed);
}

std::chrono::system_clock::time_point business_now() noexcept {
    return std::chrono::system_clock::now()
           + g_time_offset.load(std::memory_order_relaxed);
}

std::string format_business_now(const char* fmt) {
    const auto t = std::chrono::system_clock::to_time_t(business_now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof buf, fmt, &tm);
    return std::string{buf};
}

} // namespace caf_plugin_system
