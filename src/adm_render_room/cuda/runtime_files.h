#pragma once

#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <windows.h>

namespace room_cuda {
inline void runtime_anchor() {}

inline std::filesystem::path target_module_path(int major, int minor) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&runtime_anchor),
                            &module))
        throw std::runtime_error("Cannot locate the ROOM CUDA runtime");
    std::array<wchar_t, 32768> path{};
    const auto used = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!used || used >= path.size())
        throw std::runtime_error("Cannot locate the ROOM CUDA runtime directory");
    return std::filesystem::path(std::wstring(path.data(), used)).parent_path() / "room_cuda" /
           ("target-" ROOM_CUDA_TARGET_ID "-sm" + std::to_string(major) + std::to_string(minor) + ".ptx");
}
} // namespace room_cuda
