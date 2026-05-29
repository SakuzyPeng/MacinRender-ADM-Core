#include <array>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>

#include "adm/audio_io.h"
#include "adm/errors.h"
#if MR_ADM_ENABLE_IAMF
#include "iamf_aom_bridge.h"
#endif

namespace mradm::audio {

namespace {

#if MR_ADM_ENABLE_IAMF
constexpr uint32_t k_sample_rate = 48000U;
#endif

// Shell-safe single-quote wrapping for POSIX paths.
std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') { out += "'\\''"; }
        else { out += c; }
    }
    return out + "'";
}

// Run a shell command; capture combined stdout+stderr for error reporting.
Result<void> run_packager_cmd(const std::string& cmd) {
    std::string captured;
    // NOLINTNEXTLINE(cert-env33-c)
    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (pipe == nullptr) {
        return make_error(ErrorCode::io_error, "failed to launch packager command");
    }
    std::array<char, 256> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        captured += buf.data();
        if (captured.size() > 4096U) { captured += "...(truncated)"; break; }
    }
    const int rc = pclose(pipe);
    if (rc != 0) {
        while (!captured.empty() && captured.back() == '\n') { captured.pop_back(); }
        return make_error(ErrorCode::io_error, "packager command failed: " + captured);
    }
    return {};
}

bool program_exists(const char* prog) {
    std::string cmd = "command -v ";
    cmd += prog;
    cmd += " >/dev/null 2>&1";
    // NOLINTNEXTLINE(cert-env33-c)
    return std::system(cmd.c_str()) == 0;
}

} // namespace

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

IamfMp4PackagerInfo detect_iamf_mp4_packager() {
    if (program_exists("mp4box") || program_exists("MP4Box")) {
        return {IamfMp4PackagerKind::mp4box, -1};
    }

    FILE* pipe = popen("ffmpeg -version 2>&1", "r");
    if (pipe == nullptr) { return {}; }
    std::array<char, 256> line{};
    IamfMp4PackagerInfo info;
    if (fgets(line.data(), static_cast<int>(line.size()), pipe) != nullptr) {
        int maj = 0;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        if (sscanf(line.data(), "ffmpeg version %d.", &maj) == 1 && maj > 0) {
            info = {IamfMp4PackagerKind::ffmpeg, maj};
        }
    }
    pclose(pipe);
    return info;
}

bool iamf_mp4_packager_available() {
    return detect_iamf_mp4_packager().kind != IamfMp4PackagerKind::none;
}

Result<void> package_iamf_to_mp4(const std::string& iamf_path, const std::string& mp4_path) {
    const auto info = detect_iamf_mp4_packager();

    if (info.kind == IamfMp4PackagerKind::mp4box) {
        const std::string cmd =
            "mp4box -add " + shell_quote(iamf_path) + " -new " + shell_quote(mp4_path);
        return run_packager_cmd(cmd);
    }

    if (info.kind == IamfMp4PackagerKind::ffmpeg) {
        const std::string cmd =
            "ffmpeg -y -i " + shell_quote(iamf_path) + " -c copy " + shell_quote(mp4_path);
        return run_packager_cmd(cmd);
    }

    return make_error(ErrorCode::unsupported,
                      "IAMF-to-MP4 packaging requires mp4box (GPAC) or ffmpeg in PATH; "
                      "install GPAC: https://gpac.io");
}

} // namespace mradm::audio
