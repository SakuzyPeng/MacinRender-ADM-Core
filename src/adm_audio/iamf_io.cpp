#include <array>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "adm/audio_io.h"
#include "adm/errors.h"
#if MR_ADM_ENABLE_IAMF
#include "iamf_aom_bridge.h"
#endif

namespace mradm::audio {

namespace {

#if MR_ADM_ENABLE_IAMF
constexpr uint32_t k_sample_rate = 48000U;

// cppcheck-suppress constParameterCallback
int iamf_should_cancel(void* user_data) {
    if (user_data == nullptr) {
        return 0;
    }
    const auto* cancel_token = static_cast<const std::stop_token*>(user_data);
    return cancel_token->stop_requested() ? 1 : 0;
}

std::string join_csv(const std::vector<std::string>& values) {
    std::string out;
    for (const auto& value : values) {
        if (!out.empty()) {
            out += ",";
        }
        out += value;
    }
    return out;
}
#endif

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
                             std::optional<double> peak_dbtp,
                             const std::stop_token& cancel_token,
                             const std::vector<std::string>& scalable_layers) {
#if MR_ADM_ENABLE_IAMF
    if (cancel_token.stop_requested()) {
        return make_error(ErrorCode::cancelled, "render cancelled", "path=" + iamf_path);
    }
    if (layout_id == "9.1.6" || layout_id == "atmos916") {
        return make_error(ErrorCode::unsupported,
                          "IAMF 9.1.6 output is disabled: the official AOM bridge requires expanded/Base-Enhanced "
                          "IAMF for this layout, which is not currently compatible enough for release output");
    }

    auto reader_res = FloatWavReader::open(src_path);
    if (!reader_res) {
        return mradm::unexpected{reader_res.error()};
    }
    if (reader_res->sample_rate() != k_sample_rate) {
        return make_error(ErrorCode::invalid_argument,
                          "IAMF output requires 48000 Hz; got " + std::to_string(reader_res->sample_rate()));
    }

    const uint32_t bridge_abi = mr_iamf_aom_bridge_abi_version();
    if (bridge_abi == 0U || bridge_abi > MR_IAMF_AOM_BRIDGE_ABI_VERSION) {
        return make_error(ErrorCode::io_error, "AOM IAMF bridge ABI version mismatch");
    }
    if (!scalable_layers.empty() && bridge_abi < 3U) {
        return make_error(ErrorCode::unsupported, "IAMF scalable layers require AOM bridge ABI v3 or newer");
    }
    const std::string layers_csv = join_csv(scalable_layers);

    MrIamfAomEncodeOptions options{};
    options.abi_version = bridge_abi >= 3U ? 3U : (bridge_abi >= 2U ? 2U : 1U);
    options.input_wav_path = src_path.c_str();
    options.output_iamf_path = iamf_path.c_str();
    options.layout_id = layout_id.c_str();
    options.opus_bitrate_per_ch_kbps = bitrate_per_ch_kbps;
    options.profile = MR_IAMF_AOM_PROFILE_AUTO;
    options.has_loudness_lufs = loudness_lufs.has_value() ? 1 : 0;
    options.loudness_lufs = loudness_lufs.value_or(0.0);
    options.has_peak_dbtp = peak_dbtp.has_value() ? 1 : 0;
    options.peak_dbtp = peak_dbtp.value_or(0.0);
    if (bridge_abi >= 2U) {
        options.struct_size = sizeof(options);
        options.should_cancel = iamf_should_cancel;
        options.cancel_user_data = const_cast<std::stop_token*>(&cancel_token);
    }
    if (bridge_abi >= 3U) {
        options.scalable_layers_csv = layers_csv.empty() ? nullptr : layers_csv.c_str();
    }

    std::array<char, 2048> error_buffer{};
    const int rc = mr_iamf_aom_encode_wav_to_iamf(&options, error_buffer.data(), error_buffer.size());
    if (rc == MR_IAMF_AOM_RESULT_CANCELLED || cancel_token.stop_requested()) {
        return make_error(ErrorCode::cancelled, "render cancelled", "path=" + iamf_path);
    }
    if (rc != MR_IAMF_AOM_RESULT_OK) {
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
    (void) cancel_token;
    (void) scalable_layers;
    return make_error(ErrorCode::unsupported,
                      "IAMF encoding requires MR_ADM_ENABLE_IAMF=ON and the official AOM iamf-tools bridge");
#endif
}

} // namespace mradm::audio
