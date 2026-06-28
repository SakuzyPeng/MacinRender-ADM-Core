#pragma once

// 测试用的跨平台小工具：临时目录前缀与环境变量设置。
// POSIX 测试历史上硬编码 "/tmp/" 且用 ::setenv/::unsetenv，这些在 Windows 不可用。
// 这里集中提供可移植替代，使同一份 smoke/fixture 测试可在 macOS / Linux / Windows 编译并运行。

#include <filesystem>
#include <string>

#ifdef _WIN32
#include <cstdlib>
#else
#include <cstdlib>
#endif

namespace mr_test {

// 返回系统临时目录（带尾部分隔符）的字符串前缀，可直接与文件名拼接：
//   const std::string path = mr_test::temp_prefix() + "mr_foo.wav";
[[nodiscard]] inline std::string temp_prefix() {
    return (std::filesystem::temp_directory_path() / "").string();
}

// 可移植 setenv：POSIX 用 ::setenv(overwrite=1)，Windows 用 _putenv_s。
inline int set_env(const char* name, const char* value) {
#ifdef _WIN32
    return ::_putenv_s(name, value);
#else
    return ::setenv(name, value, 1);
#endif
}

// 可移植 unsetenv：Windows 上把变量置空即视为清除。
inline int unset_env(const char* name) {
#ifdef _WIN32
    return ::_putenv_s(name, "");
#else
    return ::unsetenv(name);
#endif
}

} // namespace mr_test
