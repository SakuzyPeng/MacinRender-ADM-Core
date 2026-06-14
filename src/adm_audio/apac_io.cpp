#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "adm/audio_io.h"

#ifdef __APPLE__
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <thread>
#include <unistd.h>

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#include <sys/wait.h>
#endif

#include "adm/errors.h"

#include "audio_io_internal.h"

namespace mradm::audio {

namespace {

[[nodiscard]] uint32_t default_apac_bitrate_kbps(std::string_view layout_id, uint32_t channels) {
    if (layout_id == "4+5+0" || layout_id == "5.1.4" || layout_id == "atmos514" || layout_id == "4+7+0" ||
        layout_id == "7.1.4" || layout_id == "atmos714" || layout_id == "9.1.6" || layout_id == "atmos916" ||
        layout_id == "9+10+3" || layout_id == "22.2" || layout_id == "cicp13" || layout_id == "hoa3") {
        // Use a stable APAC default for spatial/HOA layouts:
        // 12ch 7.1.4 baseline 2048 kbps, scaled by channel count.
        return ((2048U * channels) + 6U) / 12U;
    }
    return 0;
}

#ifdef __APPLE__

[[nodiscard]] std::string status_message(std::string_view op, int err) {
    return std::string{op.data(), op.size()} + " failed (" + std::to_string(err) + ")";
}

void emit_apac_progress(ProgressSink* progress,
                        RenderOperation operation,
                        double fraction,
                        uint64_t current_frame,
                        uint64_t total_frames,
                        std::string_view message) {
    if (progress == nullptr) {
        return;
    }
    const double f = std::clamp(fraction, 0.0, 1.0);
    progress->on_progress({RenderStage::post_processing, operation, f, f, current_frame, total_frames, message});
}

// The real AudioToolbox encode. Always runs inside the forked `__apac-encode`
// subprocess (reached via run_apac_encode_child) so the parent's watchdog can
// reclaim it if the encoder spin-hangs; convert_to_apac() never calls it directly.
// on_flush_start (when set) is invoked immediately before ExtAudioFileDispose so
// the heartbeat consumer can switch to its flush-phase budget.
// NOLINTBEGIN(readability-function-size,readability-function-cognitive-complexity)
Result<void> convert_to_apac_inproc(const std::string& src_path,
                                    const std::string& apac_path,
                                    const std::string& layout_id,
                                    uint32_t bitrate_kbps,
                                    bool drc_music,
                                    ApacContainer container,
                                    const std::stop_token& cancel_token,
                                    ProgressSink* progress,
                                    RenderOperation operation,
                                    const std::function<void()>& on_flush_start) {
    if (cancel_token.stop_requested()) {
        return make_error(ErrorCode::cancelled, "render cancelled", "path=" + apac_path);
    }
    struct ApacLayout {
        AudioChannelLayoutTag tag;
        bool wav71_swap;
        uint32_t expected_channels; // 0 = any channel count accepted
    };
    std::optional<ApacLayout> al;
    if (layout_id == "binaural") {
        // CoreAudio accepts the Binaural input layout here, but afinfo may
        // report the encoded APAC track as Stereo (L R). Keep the layout id for
        // validation/metadata semantics; do not rely on APAC as a durable
        // binaural channel-layout carrier.
        al = ApacLayout{kAudioChannelLayoutTag_Binaural, false, 2U};
    } else if (layout_id == "0+2+0") {
        al = ApacLayout{kAudioChannelLayoutTag_MPEG_2_0, false, 2U};
    } else if (layout_id == "wav71") {
        al = ApacLayout{kAudioChannelLayoutTag_AudioUnit_7_1, true, 8U};
    } else if (layout_id == "hoa3") {
        al = ApacLayout{static_cast<AudioChannelLayoutTag>((190U << 16U) | 16U), false, 16U};
    } else if (layout_id == "4+5+0" || layout_id == "5.1.4" || layout_id == "atmos514") {
        al = ApacLayout{static_cast<AudioChannelLayoutTag>((195U << 16U) | 10U), false, 10U};
    } else if (layout_id == "4+7+0" || layout_id == "7.1.4" || layout_id == "atmos714") {
        al = ApacLayout{static_cast<AudioChannelLayoutTag>((192U << 16U) | 12U), false, 12U};
    } else if (layout_id == "9.1.6" || layout_id == "atmos916") {
        al = ApacLayout{static_cast<AudioChannelLayoutTag>((193U << 16U) | 16U), false, 16U};
    } else if (layout_id == "9+10+3" || layout_id == "22.2" || layout_id == "cicp13") {
        al = ApacLayout{static_cast<AudioChannelLayoutTag>((204U << 16U) | 24U), false, 24U};
    } else {
        return make_error(ErrorCode::unsupported, "APAC: unsupported layout '" + layout_id + "'", {});
    }

    auto reader_res = FloatWavReader::open(src_path);
    if (!reader_res) {
        return tl::unexpected{reader_res.error()};
    }
    auto& reader = *reader_res;
    const uint32_t channels = reader.channels();

    if (al->expected_channels > 0U && channels != al->expected_channels) {
        return make_error(ErrorCode::invalid_argument,
                          "APAC layout '" + layout_id + "' requires " + std::to_string(al->expected_channels) +
                              " channels, got " + std::to_string(channels),
                          "src=" + src_path);
    }

    if (reader.sample_rate() != 48000U) {
        return make_error(ErrorCode::invalid_argument,
                          "APAC requires 48000 Hz input, got " + std::to_string(reader.sample_rate()) + " Hz",
                          "src=" + src_path);
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr,
                                                           reinterpret_cast<const UInt8*>(apac_path.c_str()),
                                                           static_cast<CFIndex>(apac_path.size()),
                                                           static_cast<Boolean>(false));
    if (url == nullptr) {
        return make_error(ErrorCode::io_error, "APAC: CFURLCreate failed", "path=" + apac_path);
    }

    // FourCC 'apac' = 0x61706163; use hex to avoid -Wmultichar in C++.
    constexpr AudioFormatID k_apac_fmt = 0x61706163U;
    AudioStreamBasicDescription apac_asbd{};
    apac_asbd.mSampleRate = 48000.0;
    apac_asbd.mFormatID = k_apac_fmt;
    apac_asbd.mChannelsPerFrame = channels;

    AudioChannelLayout file_layout{};
    file_layout.mChannelLayoutTag = al->tag;
    const AudioFileTypeID file_type = container == ApacContainer::caf ? kAudioFileCAFType : kAudioFileMPEG4Type;

    ExtAudioFileRef ext_file = nullptr;
    OSStatus err =
        ExtAudioFileCreateWithURL(url, file_type, &apac_asbd, &file_layout, kAudioFileFlags_EraseFile, &ext_file);
    CFRelease(url);
    if (err != noErr) {
        return make_error(ErrorCode::io_error,
                          status_message("APAC: ExtAudioFileCreateWithURL", static_cast<int>(err)),
                          "path=" + apac_path);
    }

    AudioStreamBasicDescription src_asbd{};
    src_asbd.mSampleRate = 48000.0;
    src_asbd.mFormatID = kAudioFormatLinearPCM;
    src_asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    src_asbd.mChannelsPerFrame = channels;
    src_asbd.mBitsPerChannel = 32;
    src_asbd.mFramesPerPacket = 1;
    src_asbd.mBytesPerFrame = static_cast<UInt32>(sizeof(float) * channels);
    src_asbd.mBytesPerPacket = src_asbd.mBytesPerFrame;
    err = ExtAudioFileSetProperty(ext_file, kExtAudioFileProperty_ClientDataFormat, sizeof(src_asbd), &src_asbd);
    if (err != noErr) {
        ExtAudioFileDispose(ext_file);
        return make_error(
            ErrorCode::io_error, status_message("APAC: ClientDataFormat", static_cast<int>(err)), "path=" + apac_path);
    }

    // APAC-specific property IDs (not yet named in SDK headers; hex avoids -Wmultichar).
    // 'csrc'=0x63737263 (source type 7=Spatial Offline, fixed)
    // 'cdrc'=0x63647263 (DRC profile: 0=None, 1=Music)
    // 'aspf'=0x61737066 (sync packet frequency 75, fixed)
    constexpr AudioConverterPropertyID k_apac_csrc = 0x63737263U;
    constexpr AudioConverterPropertyID k_apac_cdrc = 0x63647263U;
    constexpr AudioConverterPropertyID k_apac_aspf = 0x61737066U;

    AudioConverterRef conv = nullptr;
    UInt32 conv_sz = sizeof(AudioConverterRef);
    // NOLINTNEXTLINE(bugprone-multi-level-implicit-pointer-conversion)
    err = ExtAudioFileGetProperty(ext_file, kExtAudioFileProperty_AudioConverter, &conv_sz, &conv);
    if (err != noErr || conv == nullptr) {
        ExtAudioFileDispose(ext_file);
        return make_error(ErrorCode::io_error,
                          status_message("APAC: AudioConverter lookup", static_cast<int>(err)),
                          "path=" + apac_path);
    }
    const uint32_t effective_bitrate_kbps =
        bitrate_kbps > 0U ? bitrate_kbps : default_apac_bitrate_kbps(layout_id, channels);
    if (effective_bitrate_kbps > 0U) {
        UInt32 br = effective_bitrate_kbps * 1000U;
        err = AudioConverterSetProperty(conv, kAudioConverterEncodeBitRate, sizeof(br), &br);
        if (err != noErr) {
            ExtAudioFileDispose(ext_file);
            return make_error(
                ErrorCode::io_error, status_message("APAC: EncodeBitRate", static_cast<int>(err)), "path=" + apac_path);
        }
    }
    constexpr uint32_t k_csrc_val = 7U;
    constexpr uint32_t k_aspf_val = 75U;
    const uint32_t cdrc_val = drc_music ? 1U : 0U;
    err = AudioConverterSetProperty(conv, k_apac_csrc, sizeof(k_csrc_val), &k_csrc_val);
    if (err != noErr) {
        ExtAudioFileDispose(ext_file);
        return make_error(
            ErrorCode::io_error, status_message("APAC: csrc property", static_cast<int>(err)), "path=" + apac_path);
    }
    err = AudioConverterSetProperty(conv, k_apac_cdrc, sizeof(cdrc_val), &cdrc_val);
    if (err != noErr) {
        ExtAudioFileDispose(ext_file);
        return make_error(
            ErrorCode::io_error, status_message("APAC: cdrc property", static_cast<int>(err)), "path=" + apac_path);
    }
    err = AudioConverterSetProperty(conv, k_apac_aspf, sizeof(k_aspf_val), &k_aspf_val);
    if (err != noErr) {
        ExtAudioFileDispose(ext_file);
        return make_error(
            ErrorCode::io_error, status_message("APAC: aspf property", static_cast<int>(err)), "path=" + apac_path);
    }
    CFArrayRef empty = CFArrayCreate(nullptr, nullptr, 0, nullptr);
    if (empty == nullptr) {
        ExtAudioFileDispose(ext_file);
        return make_error(ErrorCode::io_error, "APAC: CFArrayCreate failed (OOM?)", "path=" + apac_path);
    }
    // NOLINTNEXTLINE(bugprone-sizeof-expression,bugprone-multi-level-implicit-pointer-conversion)
    err = ExtAudioFileSetProperty(ext_file, kExtAudioFileProperty_ConverterConfig, sizeof(CFArrayRef), &empty);
    CFRelease(empty);
    if (err != noErr) {
        ExtAudioFileDispose(ext_file);
        return make_error(ErrorCode::io_error,
                          status_message("APAC: ConverterConfig commit", static_cast<int>(err)),
                          "path=" + apac_path);
    }

    constexpr uint64_t k_block = 4096;
    std::vector<float> buf(static_cast<std::size_t>(channels) * k_block);
    uint64_t left = reader.frame_count();
    const uint64_t total_frames = left;
    uint64_t done = 0;
    emit_apac_progress(progress, operation, 0.0, 0, total_frames, "encoding APAC");

    while (left > 0) {
        if (cancel_token.stop_requested()) {
            ExtAudioFileDispose(ext_file);
            return make_error(ErrorCode::cancelled, "render cancelled", "path=" + apac_path);
        }
        const uint64_t n = std::min(k_block, left);
        const uint64_t got = reader.read(buf.data(), n);
        if (got == 0) {
            break;
        }
        if (al->wav71_swap) {
            for (uint64_t f = 0; f < got; ++f) {
                float* fr = buf.data() + (f * channels);
                std::swap(fr[4], fr[6]);
                std::swap(fr[5], fr[7]);
            }
        }
        AudioBufferList abl{};
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = channels;
        abl.mBuffers[0].mDataByteSize = static_cast<UInt32>(got * static_cast<uint64_t>(channels) * sizeof(float));
        abl.mBuffers[0].mData = buf.data();
        err = ExtAudioFileWrite(ext_file, static_cast<UInt32>(got), &abl);
        if (err != noErr) {
            ExtAudioFileDispose(ext_file);
            return make_error(ErrorCode::io_error,
                              status_message("APAC: ExtAudioFileWrite", static_cast<int>(err)),
                              "path=" + apac_path);
        }
        left -= got;
        done += got;
        emit_apac_progress(progress,
                           operation,
                           static_cast<double>(done) / static_cast<double>(std::max<uint64_t>(1, total_frames)),
                           done,
                           total_frames,
                           "encoding APAC");
    }
    if (left != 0) {
        ExtAudioFileDispose(ext_file);
        return make_error(ErrorCode::io_error, "short read in convert_to_apac", "src=" + src_path);
    }

    if (cancel_token.stop_requested()) {
        ExtAudioFileDispose(ext_file);
        return make_error(ErrorCode::cancelled, "render cancelled", "path=" + apac_path);
    }
    // ExtAudioFileDispose flushes the encoder's end-of-stream buffers. At
    // pathological bitrates this call can spin indefinitely (100% CPU, never
    // returns); the heartbeat consumer keys its flush-phase budget off this marker.
    if (on_flush_start) {
        on_flush_start();
    }
    err = ExtAudioFileDispose(ext_file);
    if (err != noErr) {
        return make_error(ErrorCode::io_error,
                          status_message("APAC: ExtAudioFileDispose", static_cast<int>(err)),
                          "path=" + apac_path);
    }
    emit_apac_progress(progress, operation, 1.0, total_frames, total_frames, "APAC encoded");
    return {};
}
// NOLINTEND(readability-function-size,readability-function-cognitive-complexity)

// ── Parent-side subprocess machinery (stall watchdog) ─────────────────────

// Heartbeat / watchdog budgets. Calibrated against measured 22.2 / 24ch / 258s
// encodes: healthy max inter-block gap 16 ms, pathological-but-progressing 53 ms,
// legitimate flush (ExtAudioFileDispose) 3 ms. A genuine spin-hang produces an
// unbounded gap, so any budget in the wide span between "worst legitimate step"
// (tens of ms) and "infinite" works — these are deliberately generous so a slow
// machine or large file never trips them.
constexpr auto k_poll_interval = std::chrono::milliseconds{50};
constexpr auto k_startup_budget = std::chrono::seconds{30};     // spawn + AudioToolbox init → first heartbeat
constexpr auto k_write_stall_budget = std::chrono::seconds{10}; // no block progress
constexpr auto k_flush_budget = std::chrono::seconds{15};       // ExtAudioFileDispose

// Watchdog budgets are overridable via env (milliseconds) for tuning on unusual
// hardware and for deterministic, fast stall tests.
std::chrono::milliseconds budget_from_env(const char* var, std::chrono::milliseconds fallback) {
    if (const char* v = ::getenv(var)) { // NOLINT(concurrency-mt-unsafe)
        char* end = nullptr;
        const long ms = std::strtol(v, &end, 10);
        if (end != v && ms > 0) {
            return std::chrono::milliseconds{ms};
        }
    }
    return fallback;
}

// Locate the trusted `mradm` helper that understands `__apac-encode`. Only an
// explicit override or a binary co-located with our own image is accepted:
// dladdr on our own code gives the path of the image containing it (the mradm
// executable in a static CLI build, or the shared library in a dylib build), and
// in both cases the bundled mradm sits next to it. PATH is deliberately NOT
// searched — a stray user-installed mradm could be a different version whose
// heartbeat protocol does not match.
std::string discover_apac_helper() {
    const auto usable = [](const std::string& p) { return !p.empty() && ::access(p.c_str(), X_OK) == 0; };

    if (const char* override_path = ::getenv("MRADM_APAC_HELPER")) { // NOLINT(concurrency-mt-unsafe)
        if (usable(override_path)) {
            return override_path;
        }
    }

    Dl_info info{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::dladdr(reinterpret_cast<const void*>(&convert_to_apac), &info) != 0 && info.dli_fname != nullptr) {
        const std::filesystem::path image{info.dli_fname};
        const auto sibling = (image.parent_path() / "mradm").string();
        if (usable(sibling)) {
            return sibling;
        }
    }

    uint32_t self_sz = 0;
    _NSGetExecutablePath(nullptr, &self_sz);
    std::string self(self_sz, '\0');
    if (self_sz > 0 && _NSGetExecutablePath(self.data(), &self_sz) == 0) {
        self.resize(std::strlen(self.c_str()));
        if (std::filesystem::path{self}.filename() == "mradm" && usable(self)) {
            return self;
        }
    }

    return {};
}

enum class WatchdogPhase : std::uint8_t { startup, writing, flushing };

struct SubprocessOutcome {
    bool ok{false};
    bool cancelled{false};
    bool stalled{false};
    ErrorCode code{ErrorCode::io_error};
    std::string message;
};

// Parse a complete protocol line emitted by run_apac_encode_child.
void handle_protocol_line(const std::string& line,
                          WatchdogPhase& phase,
                          std::chrono::steady_clock::time_point& last_progress,
                          std::chrono::steady_clock::time_point& flush_start,
                          ProgressSink* progress,
                          RenderOperation operation,
                          ErrorCode& child_code,
                          std::string& child_message) {
    const auto now = std::chrono::steady_clock::now();
    if (line.starts_with("P ")) {
        unsigned long long cur = 0;
        unsigned long long total = 0;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        if (std::sscanf(line.c_str(), "P %llu %llu", &cur, &total) == 2) {
            phase = WatchdogPhase::writing;
            last_progress = now;
            if (progress != nullptr) {
                const double f = total > 0 ? static_cast<double>(cur) / static_cast<double>(total) : 0.0;
                progress->on_progress({RenderStage::post_processing,
                                       operation,
                                       std::clamp(f, 0.0, 1.0),
                                       std::clamp(f, 0.0, 1.0),
                                       cur,
                                       total,
                                       "encoding APAC"});
            }
        }
    } else if (line == "F") {
        phase = WatchdogPhase::flushing;
        flush_start = now;
    } else if (line.starts_with("E ")) {
        int code = static_cast<int>(ErrorCode::io_error);
        const auto msg_pos = line.find(' ', 2);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        std::sscanf(line.c_str(), "E %d", &code);
        child_code = static_cast<ErrorCode>(code);
        child_message = msg_pos != std::string::npos ? line.substr(msg_pos + 1) : std::string{};
    }
    // Non-protocol lines (stray logs) are ignored.
}

// Fork the helper, stream its heartbeat, and enforce the two-phase stall budget.
// On stall or external cancel the whole child process group is SIGKILLed — the
// only way to reclaim a spin-wedged AudioToolbox encoder thread.
// NOLINTBEGIN(readability-function-size,readability-function-cognitive-complexity)
SubprocessOutcome run_apac_subprocess(const std::string& helper,
                                      const std::vector<std::string>& args,
                                      const std::stop_token& cancel_token,
                                      ProgressSink* progress,
                                      RenderOperation operation) {
    std::array<int, 2> fds{-1, -1};
    if (::pipe(fds.data()) != 0) {
        return {.ok = false, .message = "APAC: pipe() failed"};
    }

    // Build argv before forking: the c_str() pointers stay valid in the child
    // (same address space), so the child path touches only async-signal-safe
    // calls — important since the engine may run inside a multithreaded host.
    std::vector<const char*> argv;
    argv.reserve(args.size() + 2U);
    argv.push_back(helper.c_str());
    for (const auto& a : args) {
        argv.push_back(a.c_str());
    }
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return {.ok = false, .message = "APAC: fork() failed"};
    }

    if (pid == 0) {
        ::setpgid(0, 0);
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[1]);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) — execv signature requires char* const*
        ::execv(helper.c_str(), const_cast<char* const*>(argv.data()));
        ::_exit(127);
    }

    ::setpgid(pid, pid);
    ::close(fds[1]);
    const int flags = ::fcntl(fds[0], F_GETFL, 0);
    if (flags >= 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        ::fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
    }

    const auto startup_budget = budget_from_env("MRADM_APAC_STARTUP_MS", k_startup_budget);
    const auto write_stall_budget = budget_from_env("MRADM_APAC_WRITE_STALL_MS", k_write_stall_budget);
    const auto flush_budget = budget_from_env("MRADM_APAC_FLUSH_BUDGET_MS", k_flush_budget);

    const auto spawn_time = std::chrono::steady_clock::now();
    auto phase = WatchdogPhase::startup;
    auto last_progress = spawn_time;
    auto flush_start = spawn_time;
    ErrorCode child_code = ErrorCode::io_error;
    std::string child_message;
    std::string line_buf;

    SubprocessOutcome out;
    bool killed = false;
    bool reaped = false;
    int status = 0;

    const auto drain = [&] {
        std::array<char, 512> buf{};
        for (;;) {
            const ssize_t n = ::read(fds[0], buf.data(), buf.size());
            if (n <= 0) {
                break;
            }
            line_buf.append(buf.data(), static_cast<std::size_t>(n));
            std::size_t nl = 0;
            while ((nl = line_buf.find('\n')) != std::string::npos) {
                handle_protocol_line(line_buf.substr(0, nl),
                                     phase,
                                     last_progress,
                                     flush_start,
                                     progress,
                                     operation,
                                     child_code,
                                     child_message);
                line_buf.erase(0, nl + 1);
            }
        }
    };

    const auto kill_child = [&] {
        if (::kill(-pid, SIGKILL) != 0) {
            ::kill(pid, SIGKILL);
        }
        killed = true;
    };

    for (;;) {
        drain();

        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            reaped = true;
            break;
        }
        if (done < 0 && errno != EINTR) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (cancel_token.stop_requested()) {
            out.cancelled = true;
            kill_child();
            break;
        }
        bool stalled = false;
        switch (phase) {
        case WatchdogPhase::startup:
            stalled = now - spawn_time > startup_budget;
            break;
        case WatchdogPhase::writing:
            stalled = now - last_progress > write_stall_budget;
            break;
        case WatchdogPhase::flushing:
            stalled = now - flush_start > flush_budget;
            break;
        }
        if (stalled) {
            out.stalled = true;
            kill_child();
            break;
        }
        std::this_thread::sleep_for(k_poll_interval);
    }

    if (killed && !reaped) {
        ::waitpid(pid, &status, 0);
        reaped = true;
    }
    drain();
    ::close(fds[0]);

    if (out.cancelled || out.stalled) {
        return out;
    }
    const int code = (reaped && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    if (code == 0) {
        out.ok = true;
        return out;
    }
    out.ok = false;
    out.code = child_code;
    out.message = child_message;
    return out;
}
// NOLINTEND(readability-function-size,readability-function-cognitive-complexity)

#endif // __APPLE__

} // namespace

bool apac_encoding_available() {
#ifdef __APPLE__
    return true;
#else
    return false;
#endif
}

Result<void> run_apac_encode_child(const std::string& src_path,
                                   const std::string& apac_path,
                                   const std::string& layout_id,
                                   uint32_t bitrate_kbps,
                                   bool drc_music,
                                   bool caf_container) {
#ifndef __APPLE__
    (void) src_path;
    (void) apac_path;
    (void) layout_id;
    (void) bitrate_kbps;
    (void) drc_music;
    (void) caf_container;
    return make_error(ErrorCode::unsupported, "APAC encoding requires macOS (AudioToolbox)", {});
#else
    struct HeartbeatSink final : ProgressSink {
        void on_progress(const ProgressEvent& event) override {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
            std::printf("P %llu %llu\n",
                        static_cast<unsigned long long>(event.current_frame),
                        static_cast<unsigned long long>(event.total_frames));
            std::fflush(stdout);
        }
    } sink;
    const auto on_flush_start = [] {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        std::printf("F\n");
        std::fflush(stdout);
        if (::getenv("MRADM_APAC_TEST_FORCE_STALL") != nullptr) { // NOLINT(concurrency-mt-unsafe)
            // Test hook: emulate the AudioToolbox flush spin-hang (100% CPU, never
            // returns) so the parent's flush-budget watchdog and SIGKILL path can be
            // exercised deterministically. The parent reclaims this process.
            volatile uint64_t spin = 0;
            for (;;) {
                spin = spin + 1;
            }
        }
    };
    const auto container = caf_container ? ApacContainer::caf : ApacContainer::mpeg4;
    const std::stop_token no_cancel; // parent kills this process to cancel
    return convert_to_apac_inproc(src_path,
                                  apac_path,
                                  layout_id,
                                  bitrate_kbps,
                                  drc_music,
                                  container,
                                  no_cancel,
                                  &sink,
                                  RenderOperation::encode_apac,
                                  on_flush_start);
#endif
}

// NOLINTBEGIN(readability-function-size)
Result<void> convert_to_apac(const std::string& src_path,
                             const std::string& apac_path,
                             const std::string& layout_id,
                             uint32_t bitrate_kbps,
                             bool drc_music,
                             ApacContainer container,
                             const std::stop_token& cancel_token,
                             ProgressSink* progress,
                             RenderOperation operation) {
#ifndef __APPLE__
    (void) src_path;
    (void) apac_path;
    (void) layout_id;
    (void) bitrate_kbps;
    (void) drc_music;
    (void) container;
    (void) cancel_token;
    (void) progress;
    (void) operation;
    return make_error(ErrorCode::unsupported, "APAC encoding requires macOS (AudioToolbox)", {});
#else
    if (cancel_token.stop_requested()) {
        return make_error(ErrorCode::cancelled, "render cancelled", "path=" + apac_path);
    }

    const std::string helper = discover_apac_helper();
    if (helper.empty()) {
        // No trusted helper. We deliberately do NOT fall back to an unguarded
        // in-process encode — at pathological bitrates that can spin-hang the
        // host with no way to recover. Ship the mradm binary next to the library
        // (or point MRADM_APAC_HELPER at it) to enable APAC encoding.
        return make_error(ErrorCode::unsupported,
                          "APAC 编码需要随附的 mradm helper:请与库放在同一目录,或设置 MRADM_APAC_HELPER",
                          "path=" + apac_path);
    }

    const std::vector<std::string> args{"__apac-encode",
                                        "--in",
                                        src_path,
                                        "--out",
                                        apac_path,
                                        "--layout",
                                        layout_id,
                                        "--bitrate",
                                        std::to_string(bitrate_kbps),
                                        "--drc",
                                        drc_music ? "1" : "0",
                                        "--container",
                                        container == ApacContainer::caf ? "caf" : "mpeg4"};

    // Progress (including the terminal "APAC encoded" at current==total) is
    // forwarded verbatim from the child's heartbeat, so nothing to emit here.
    const auto outcome = run_apac_subprocess(helper, args, cancel_token, progress, operation);
    if (outcome.ok) {
        return {};
    }

    // Best-effort: drop the partial / wedged output so we never leave a corrupt .m4a.
    std::error_code ec;
    std::filesystem::remove(apac_path, ec);

    if (outcome.cancelled) {
        return make_error(ErrorCode::cancelled, "render cancelled", "path=" + apac_path);
    }
    if (outcome.stalled) {
        return make_error(ErrorCode::io_error,
                          "APAC 编码在此码率下停滞(编码器无响应),请降低码率后重试",
                          "path=" + apac_path + " bitrate=" + std::to_string(bitrate_kbps) + "kbps");
    }
    return make_error(
        outcome.code, outcome.message.empty() ? "APAC 子进程编码失败" : outcome.message, "path=" + apac_path);
#endif
}
// NOLINTEND(readability-function-size)

} // namespace mradm::audio
