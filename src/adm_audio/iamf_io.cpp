#include <array>
#include <cstdint>
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

} // namespace mradm::audio
