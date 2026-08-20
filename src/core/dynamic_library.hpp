#pragma once
#include <string>
#include <filesystem>
#include <optional>

// ------------------------------------------------------------------
// 跨平台动态库加载封装
// Linux/macOS: dlopen / dlsym / dlclose
// Windows:     LoadLibraryW / GetProcAddress / FreeLibrary
// ------------------------------------------------------------------
class DynamicLibrary {
public:
    static std::optional<DynamicLibrary> open(const std::filesystem::path& path);
    ~DynamicLibrary();

    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    // 获取符号地址，转换为函数指针（Func 为函数指针类型，如 void(*)()）
    template<typename Func>
    Func symbol(const std::string& name) const {
        return reinterpret_cast<Func>(symbol_raw(name));
    }

    bool valid() const { return handle_ != nullptr; }

private:
    explicit DynamicLibrary(void* handle) : handle_(handle) {}
    void* symbol_raw(const std::string& name) const;
    void* handle_ = nullptr;
};
