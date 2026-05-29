#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "adm/audio_io.h"
#include "adm/errors.h"
#if MR_ADM_ENABLE_IAMF
#include "iamf_aom_bridge.h"
#endif

// ── Platform process helpers ──────────────────────────────────────────────
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace mradm::audio {

namespace {

#if MR_ADM_ENABLE_IAMF
constexpr uint32_t k_sample_rate = 48000U;
#endif

// ── Cross-platform process helpers ───────────────────────────────────────

struct RunResult {
    int code{-1};
    std::string output;
};

constexpr std::size_t k_max_process_output = 4096U;
constexpr const char* k_process_output_truncated = "...(truncated)";

void append_process_output(std::string& output, const char* data, std::size_t size) {
    if (output.size() >= k_max_process_output) {
        if (output.find(k_process_output_truncated) == std::string::npos) {
            output += k_process_output_truncated;
        }
        return;
    }

    const std::size_t remaining = k_max_process_output - output.size();
    output.append(data, std::min(size, remaining));
    if (size > remaining) {
        output += k_process_output_truncated;
    }
}

#if defined(_WIN32)

static std::wstring to_wide(const std::string& s) {
    if (s.empty()) { return {}; }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) { return {}; }
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Per MSDN CommandLineToArgvW quoting rules.
static std::wstring win_quote_arg(const std::wstring& arg) {
    bool needs_quote = arg.empty();
    for (wchar_t c : arg) {
        if (c == L' ' || c == L'\t' || c == L'"') { needs_quote = true; break; }
    }
    if (!needs_quote) { return arg; }

    std::wstring out = L"\"";
    int bs = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++bs;
        } else if (c == L'"') {
            for (int i = 0; i < bs * 2; ++i) { out += L'\\'; }
            out += L"\\\"";
            bs = 0;
        } else {
            for (; bs > 0; --bs) { out += L'\\'; }
            out += c;
        }
    }
    for (int i = 0; i < bs * 2; ++i) { out += L'\\'; }
    return out + L'"';
}

std::string find_in_path(std::initializer_list<const char*> candidates) {
    for (const char* name : candidates) {
        const auto wname = to_wide(name);
        if (wname.empty()) { continue; }
        wchar_t found[MAX_PATH]{};
        if (SearchPathW(nullptr, wname.c_str(), L".exe", MAX_PATH, found, nullptr) > 0) { return name; }
    }
    return {};
}

RunResult run_process(const std::string& prog, const std::vector<std::string>& args) {
    std::wstring cmdline = win_quote_arg(to_wide(prog));
    for (const auto& a : args) {
        cmdline += L' ';
        cmdline += win_quote_arg(to_wide(a));
    }
    std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back(L'\0');

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE pipe_r = nullptr;
    HANDLE pipe_w = nullptr;
    if (CreatePipe(&pipe_r, &pipe_w, &sa, 0) == 0) { return {-1, "CreatePipe failed"}; }
    SetHandleInformation(pipe_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.hStdOutput = pipe_w;
    si.hStdError = pipe_w;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.dwFlags = STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr,
                                   TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(pipe_w);

    if (ok == 0) {
        CloseHandle(pipe_r);
        return {-1, "CreateProcessW failed"};
    }

    std::string output;
    std::array<char, 512> buf{};
    DWORD n = 0;
    while (ReadFile(pipe_r, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) != 0 && n > 0) {
        append_process_output(output, buf.data(), n);
    }
    CloseHandle(pipe_r);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return {static_cast<int>(exit_code), std::move(output)};
}

#else // POSIX ---------------------------------------------------------------

std::string find_in_path(std::initializer_list<const char*> candidates) {
    const char* path_env = ::getenv("PATH"); // NOLINT(concurrency-mt-unsafe)
    if (path_env == nullptr) { return {}; }
    const std::string path_str(path_env);

    for (const char* name : candidates) {
        std::size_t start = 0;
        while (start <= path_str.size()) {
            const auto end = path_str.find(':', start);
            const auto dir_end = (end == std::string::npos) ? path_str.size() : end;
            if (dir_end > start) {
                const auto candidate = path_str.substr(start, dir_end - start) + "/" + name;
                if (::access(candidate.c_str(), X_OK) == 0) { return name; }
            }
            if (end == std::string::npos) { break; }
            start = end + 1;
        }
    }
    return {};
}

RunResult run_process(const std::string& prog, const std::vector<std::string>& args) {
    std::array<int, 2> fds{-1, -1};
    if (::pipe(fds.data()) != 0) { return {-1, "pipe() failed"}; }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return {-1, "fork() failed"};
    }

    if (pid == 0) {
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[1]);

        std::vector<const char*> argv;
        argv.reserve(args.size() + 2U);
        argv.push_back(prog.c_str());
        for (const auto& a : args) { argv.push_back(a.c_str()); }
        argv.push_back(nullptr);

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) — execvp signature requires char* const*
        ::execvp(prog.c_str(), const_cast<char* const*>(argv.data()));
        ::_exit(127);
    }

    ::close(fds[1]);
    std::string output;
    std::array<char, 512> buf{};
    ssize_t n = 0;
    while ((n = ::read(fds[0], buf.data(), buf.size())) > 0) {
        append_process_output(output, buf.data(), static_cast<std::size_t>(n));
    }
    ::close(fds[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return {code, std::move(output)};
}

#endif // _WIN32

} // namespace

// ── IAMF bridge ──────────────────────────────────────────────────────────

bool iamf_encoding_available() {
#if MR_ADM_ENABLE_IAMF
    return true;
#else
    return false;
#endif
}

Result<void> convert_to_iamf(const std::string& src_path,
                             const std::string& iamf_path,
                             const std::string& layout_id,
                             uint32_t bitrate_per_ch_kbps,
                             std::optional<double> loudness_lufs,
                             std::optional<double> peak_dbtp) {
#if MR_ADM_ENABLE_IAMF
    if (layout_id == "9.1.6" || layout_id == "atmos916") {
        return make_error(ErrorCode::unsupported,
                          "IAMF 9.1.6 output is disabled: the official AOM bridge requires expanded/Base-Enhanced "
                          "IAMF for this layout, which is not currently compatible enough for release output");
    }

    auto reader_res = FloatWavReader::open(src_path);
    if (!reader_res) {
        return tl::unexpected{reader_res.error()};
    }
    if (reader_res->sample_rate() != k_sample_rate) {
        return make_error(ErrorCode::invalid_argument,
                          "IAMF output requires 48000 Hz; got " + std::to_string(reader_res->sample_rate()));
    }

    if (mr_iamf_aom_bridge_abi_version() != MR_IAMF_AOM_BRIDGE_ABI_VERSION) {
        return make_error(ErrorCode::io_error, "AOM IAMF bridge ABI version mismatch");
    }

    MrIamfAomEncodeOptions options{};
    options.abi_version = MR_IAMF_AOM_BRIDGE_ABI_VERSION;
    options.input_wav_path = src_path.c_str();
    options.output_iamf_path = iamf_path.c_str();
    options.layout_id = layout_id.c_str();
    options.opus_bitrate_per_ch_kbps = bitrate_per_ch_kbps;
    options.profile = MR_IAMF_AOM_PROFILE_AUTO;
    options.has_loudness_lufs = loudness_lufs.has_value() ? 1 : 0;
    options.loudness_lufs = loudness_lufs.value_or(0.0);
    options.has_peak_dbtp = peak_dbtp.has_value() ? 1 : 0;
    options.peak_dbtp = peak_dbtp.value_or(0.0);

    std::array<char, 2048> error_buffer{};
    const int rc = mr_iamf_aom_encode_wav_to_iamf(&options, error_buffer.data(), error_buffer.size());
    if (rc != 0) {
        const std::string detail = error_buffer[0] != '\0' ? std::string(error_buffer.data()) : "unknown bridge error";
        return make_error(ErrorCode::io_error, "AOM IAMF bridge encode failed: " + detail, "path=" + iamf_path);
    }

    return {};
#else
    (void) src_path;
    (void) iamf_path;
    (void) layout_id;
    (void) bitrate_per_ch_kbps;
    (void) loudness_lufs;
    (void) peak_dbtp;
    return make_error(ErrorCode::unsupported,
                      "IAMF encoding requires MR_ADM_ENABLE_IAMF=ON and the official AOM iamf-tools bridge");
#endif
}

// ── IAMF-to-MP4 packaging ─────────────────────────────────────────────────

IamfMp4PackagerInfo detect_iamf_mp4_packager() {
    const auto mp4box = find_in_path({"mp4box", "MP4Box"});
    if (!mp4box.empty()) {
        return {IamfMp4PackagerKind::mp4box, -1, mp4box};
    }

    const auto ffmpeg = find_in_path({"ffmpeg"});
    if (ffmpeg.empty()) { return {}; }

    // Probe version: first line of `ffmpeg -version` is "ffmpeg version X.Y.Z ..."
    const auto res = run_process(ffmpeg, {"-version"});
    if (res.code != 0 || res.output.empty()) { return {}; }

    int maj = 0;
    const auto first_newline = res.output.find('\n');
    const auto first_line = res.output.substr(0, first_newline);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (std::sscanf(first_line.c_str(), "ffmpeg version %d.", &maj) == 1 && maj > 0) {
        return {IamfMp4PackagerKind::ffmpeg, maj, ffmpeg};
    }
    return {};
}

bool iamf_mp4_packager_available() {
    return detect_iamf_mp4_packager().kind != IamfMp4PackagerKind::none;
}

Result<void> package_iamf_to_mp4(const std::string& iamf_path, const std::string& mp4_path) {
    const auto info = detect_iamf_mp4_packager();

    if (info.kind == IamfMp4PackagerKind::mp4box) {
        const auto res = run_process(info.executable, {"-add", iamf_path, "-new", mp4_path});
        if (res.code != 0) {
            auto msg = res.output;
            while (!msg.empty() && msg.back() == '\n') { msg.pop_back(); }
            return make_error(ErrorCode::io_error, "mp4box packaging failed: " + msg);
        }
        return {};
    }

    if (info.kind == IamfMp4PackagerKind::ffmpeg) {
        const auto res = run_process(info.executable, {"-y", "-i", iamf_path, "-c", "copy", mp4_path});
        if (res.code != 0) {
            auto msg = res.output;
            while (!msg.empty() && msg.back() == '\n') { msg.pop_back(); }
            return make_error(ErrorCode::io_error, "ffmpeg packaging failed: " + msg);
        }
        return {};
    }

    return make_error(ErrorCode::unsupported,
                      "IAMF-to-MP4 packaging requires mp4box (GPAC) or ffmpeg in PATH; "
                      "install GPAC: https://gpac.io");
}

} // namespace mradm::audio
