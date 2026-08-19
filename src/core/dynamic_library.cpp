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
