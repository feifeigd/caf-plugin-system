#include "dynamic_library.hpp"

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

// ------------------------------------------------------------------
// POSIX (Linux / macOS)
// ------------------------------------------------------------------
#ifndef _WIN32

std::optional<DynamicLibrary> DynamicLibrary::open(const std::filesystem::path& path) {
    void* h = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) return std::nullopt;
    return DynamicLibrary(h);
}

void* DynamicLibrary::symbol_raw(const std::string& name) const {
    return ::dlsym(handle_, name.c_str());
}

DynamicLibrary::~DynamicLibrary() {
    if (handle_) ::dlclose(handle_);
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
    : handle_(other.handle_) {
    other.handle_ = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
    if (this != &other) {
        if (handle_) ::dlclose(handle_);
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

// ------------------------------------------------------------------
// Windows
// ------------------------------------------------------------------
#else

std::optional<DynamicLibrary> DynamicLibrary::open(const std::filesystem::path& path) {
    // 朴素 LoadLibraryW 定稿（历史：LoadLibraryExW(DLL_LOAD_DIR) 方案
    // 2026-08-30 曾试，当时误判与关机崩溃相关，已证伪作废）。
    // 依赖解析机制（2026-08-31 定稿）：framework_bootstrap 在
    // SetDefaultDllDirectories(DEFAULT_DIRS) 下注册 exe → plugins/*/ →
    // lib/（方案 Y），插件自带第三方依赖（如 lua_host/lua.dll）从
    // 插件子目录解析，lib/ 只放插件目录里没有的公共第三方依赖。
    HMODULE h = ::LoadLibraryW(path.wstring().c_str());
    if (!h) return std::nullopt;
    return DynamicLibrary(h);
}

void* DynamicLibrary::symbol_raw(const std::string& name) const {
    return reinterpret_cast<void*>(::GetProcAddress(
        static_cast<HMODULE>(handle_), name.c_str()));
}

DynamicLibrary::~DynamicLibrary() {
    if (handle_) ::FreeLibrary(static_cast<HMODULE>(handle_));
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
    : handle_(other.handle_) {
    other.handle_ = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
    if (this != &other) {
        if (handle_) ::FreeLibrary(static_cast<HMODULE>(handle_));
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

#endif
