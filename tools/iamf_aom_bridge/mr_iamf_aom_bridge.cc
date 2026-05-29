#include "mr_iamf_aom_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "google/protobuf/text_format.h"
#include "iamf/cli/encoder_main_lib.h"
#include "iamf/cli/proto/user_metadata.pb.h"

namespace {

struct LayoutInfo {
    std::string_view id;
    std::string_view loudspeaker_layout;
    std::string_view expanded_loudspeaker_layout;
    std::string_view sound_system;
    uint32_t substream_count;
    uint32_t coupled_substream_count;
    std::vector<std::string_view> channel_labels;
};

const LayoutInfo* find_layout(std::string_view id) {
    static const std::vector<LayoutInfo> k_layouts{
        {"0+2+0",
         "LOUDSPEAKER_LAYOUT_STEREO",
         "",
         "SOUND_SYSTEM_A_0_2_0",
         1,
         1,
         {"CHANNEL_LABEL_L_2", "CHANNEL_LABEL_R_2"}},
        {"binaural",
         "LOUDSPEAKER_LAYOUT_STEREO",
         "",
         "SOUND_SYSTEM_A_0_2_0",
         1,
         1,
         {"CHANNEL_LABEL_L_2", "CHANNEL_LABEL_R_2"}},
        {"0+5+0",
         "LOUDSPEAKER_LAYOUT_5_1_CH",
         "",
         "SOUND_SYSTEM_B_0_5_0",
         4,
         2,
         {"CHANNEL_LABEL_L_5",
          "CHANNEL_LABEL_R_5",
          "CHANNEL_LABEL_CENTRE",
          "CHANNEL_LABEL_LFE",
          "CHANNEL_LABEL_LS_5",
          "CHANNEL_LABEL_RS_5"}},
        {"2+5+0",
         "LOUDSPEAKER_LAYOUT_5_1_2_CH",
         "",
         "SOUND_SYSTEM_C_2_5_0",
         5,
         3,
         {"CHANNEL_LABEL_L_5",
          "CHANNEL_LABEL_R_5",
          "CHANNEL_LABEL_CENTRE",
          "CHANNEL_LABEL_LFE",
          "CHANNEL_LABEL_LS_5",
          "CHANNEL_LABEL_RS_5",
          "CHANNEL_LABEL_LTF_2",
          "CHANNEL_LABEL_RTF_2"}},
        {"4+5+0",
         "LOUDSPEAKER_LAYOUT_5_1_4_CH",
         "",
         "SOUND_SYSTEM_D_4_5_0",
         6,
         4,
         {"CHANNEL_LABEL_L_5",
          "CHANNEL_LABEL_R_5",
          "CHANNEL_LABEL_CENTRE",
          "CHANNEL_LABEL_LFE",
          "CHANNEL_LABEL_LS_5",
          "CHANNEL_LABEL_RS_5",
          "CHANNEL_LABEL_LTF_4",
          "CHANNEL_LABEL_RTF_4",
          "CHANNEL_LABEL_LTB_4",
          "CHANNEL_LABEL_RTB_4"}},
        {"wav71",
         "LOUDSPEAKER_LAYOUT_7_1_CH",
         "",
         "SOUND_SYSTEM_I_0_7_0",
         5,
         3,
         {"CHANNEL_LABEL_L_7",
          "CHANNEL_LABEL_R_7",
          "CHANNEL_LABEL_CENTRE",
          "CHANNEL_LABEL_LFE",
          "CHANNEL_LABEL_LRS_7",
          "CHANNEL_LABEL_RRS_7",
          "CHANNEL_LABEL_LSS_7",
          "CHANNEL_LABEL_RSS_7"}},
        {"4+7+0",
         "LOUDSPEAKER_LAYOUT_7_1_4_CH",
         "",
         "SOUND_SYSTEM_J_4_7_0",
         7,
         5,
         {"CHANNEL_LABEL_L_7",
          "CHANNEL_LABEL_R_7",
          "CHANNEL_LABEL_CENTRE",
          "CHANNEL_LABEL_LFE",
          "CHANNEL_LABEL_LSS_7",
          "CHANNEL_LABEL_RSS_7",
          "CHANNEL_LABEL_LRS_7",
          "CHANNEL_LABEL_RRS_7",
          "CHANNEL_LABEL_LTF_4",
          "CHANNEL_LABEL_RTF_4",
          "CHANNEL_LABEL_LTB_4",
          "CHANNEL_LABEL_RTB_4"}},
    };
    const auto it = std::ranges::find_if(k_layouts, [&](const LayoutInfo& layout) { return layout.id == id; });
    return it == k_layouts.end() ? nullptr : &*it;
}

std::string q78_field(const char* name, bool has_value, double value) {
    if (!has_value || !std::isfinite(value)) {
        return {};
    }
    return absl::StrCat(name, ": ", static_cast<int32_t>(std::lrint(value * 256.0)), "\n");
}

std::string build_metadata_text(const MrIamfAomEncodeOptions& options,
                                const LayoutInfo& layout,
                                std::string_view wav_filename,
                                std::string_view file_prefix) {
    const uint32_t bitrate = options.opus_bitrate_per_ch_kbps == 0 ? 64000 : options.opus_bitrate_per_ch_kbps * 1000U;
    std::ostringstream out;
    out << "test_vector_metadata {\n"
        << "  file_name_prefix: \"" << file_prefix << "\"\n"
        << "  is_valid: true\n"
        << "  is_valid_to_decode: true\n"
        << "  validate_user_loudness: false\n"
        << "  partition_mix_gain_parameter_blocks: false\n"
        << "}\n"
        << "encoder_control_metadata { add_build_information_tag: false output_rendered_file_format: "
           "OUTPUT_FORMAT_WAV_BIT_DEPTH_AUTOMATIC }\n"
        << "ia_sequence_header_metadata { primary_profile: PROFILE_VERSION_SIMPLE additional_profile: "
           "PROFILE_VERSION_SIMPLE }\n"
        << "codec_config_metadata {\n"
        << "  codec_config_id: 200\n"
        << "  codec_config {\n"
        << "    codec_id: CODEC_ID_OPUS\n"
        << "    num_samples_per_frame: 960\n"
        << "    audio_roll_distance: -4\n"
        << "    decoder_config_opus { version: 1 pre_skip: 312 input_sample_rate: 48000 "
           "opus_encoder_metadata { target_bitrate_per_channel: "
        << bitrate << " application: APPLICATION_AUDIO use_float_api: true } }\n"
        << "  }\n"
        << "}\n"
        << "audio_element_metadata {\n"
        << "  audio_element_id: 300\n"
        << "  audio_element_type: AUDIO_ELEMENT_CHANNEL_BASED\n"
        << "  reserved: 0\n"
        << "  codec_config_id: 200\n";
    for (uint32_t sid = 0; sid < layout.substream_count; ++sid) {
        out << "  audio_substream_ids: " << sid << "\n";
    }
    out << "  scalable_channel_layout_config { reserved: 0 channel_audio_layer_configs { loudspeaker_layout: "
        << layout.loudspeaker_layout << " output_gain_is_present_flag: 0 recon_gain_is_present_flag: 0 reserved_a: 0 "
        << "substream_count: " << layout.substream_count
        << " coupled_substream_count: " << layout.coupled_substream_count;
    if (!layout.expanded_loudspeaker_layout.empty()) {
        out << " expanded_loudspeaker_layout: " << layout.expanded_loudspeaker_layout;
    }
    out << " } }\n"
        << "}\n"
        << "mix_presentation_metadata {\n"
        << "  mix_presentation_id: 42\n"
        << "  annotations_language: \"en-us\"\n"
        << "  localized_presentation_annotations: \"mradm_mix\"\n"
        << "  sub_mixes {\n"
        << "    audio_elements { audio_element_id: 300 localized_element_annotations: \"bed\" "
           "rendering_config { headphones_rendering_mode: HEADPHONES_RENDERING_MODE_STEREO } "
           "element_mix_gain { param_definition { parameter_id: 100 parameter_rate: 48000 param_definition_mode: 1 "
           "reserved: 0 } default_mix_gain: 0 } }\n"
        << "    output_mix_gain { param_definition { parameter_id: 101 parameter_rate: 48000 param_definition_mode: 1 "
           "reserved: 0 } default_mix_gain: 0 }\n"
        << "    layouts { loudness_layout { layout_type: LAYOUT_TYPE_LOUDSPEAKERS_SS_CONVENTION ss_layout { "
           "sound_system: SOUND_SYSTEM_A_0_2_0 reserved: 0 } } loudness { info_type_bit_masks: [] "
        << q78_field("integrated_loudness", options.has_loudness_lufs != 0, options.loudness_lufs)
        << q78_field("true_peak", options.has_peak_dbtp != 0, options.peak_dbtp) << "} }\n";
    if (layout.sound_system != "SOUND_SYSTEM_A_0_2_0") {
        out << "    layouts { loudness_layout { layout_type: LAYOUT_TYPE_LOUDSPEAKERS_SS_CONVENTION ss_layout { "
               "sound_system: "
            << layout.sound_system << " reserved: 0 } } loudness { info_type_bit_masks: [] "
            << q78_field("integrated_loudness", options.has_loudness_lufs != 0, options.loudness_lufs)
            << q78_field("true_peak", options.has_peak_dbtp != 0, options.peak_dbtp) << "} }\n";
    }
    out << "  }\n"
        << "}\n"
        << "audio_frame_metadata {\n"
        << "  wav_filename: \"" << wav_filename << "\"\n"
        << "  samples_to_trim_at_end_includes_padding: false\n"
        << "  samples_to_trim_at_start_includes_codec_delay: false\n"
        << "  audio_element_id: 300\n";
    for (size_t i = 0; i < layout.channel_labels.size(); ++i) {
        out << "  channel_metadatas { channel_id: " << i << " channel_label: " << layout.channel_labels[i] << " }\n";
    }
    out << "}\n"
        << "temporal_delimiter_metadata { enable_temporal_delimiters: false }\n";
    return out.str();
}

void write_error(char* error_buffer, size_t error_buffer_size, std::string_view message) {
    if (error_buffer == nullptr || error_buffer_size == 0) {
        return;
    }
    const size_t n = std::min(error_buffer_size - 1, message.size());
    std::copy_n(message.data(), n, error_buffer);
    error_buffer[n] = '\0';
}

int fail(char* error_buffer, size_t error_buffer_size, std::string_view message) {
    write_error(error_buffer, error_buffer_size, message);
    return -1;
}

} // namespace

uint32_t mr_iamf_aom_bridge_abi_version(void) {
    return MR_IAMF_AOM_BRIDGE_ABI_VERSION;
}

int mr_iamf_aom_encode_wav_to_iamf(const MrIamfAomEncodeOptions* options,
                                   char* error_buffer,
                                   size_t error_buffer_size) {
    if (options == nullptr) {
        return fail(error_buffer, error_buffer_size, "options must not be null");
    }
    if (options->abi_version != MR_IAMF_AOM_BRIDGE_ABI_VERSION) {
        return fail(error_buffer, error_buffer_size, "bridge ABI version mismatch");
    }
    if (options->input_wav_path == nullptr || options->output_iamf_path == nullptr || options->layout_id == nullptr) {
        return fail(error_buffer, error_buffer_size, "input_wav_path, output_iamf_path, and layout_id are required");
    }

    const LayoutInfo* layout = find_layout(options->layout_id);
    if (layout == nullptr) {
        return fail(error_buffer, error_buffer_size, absl::StrCat("unsupported IAMF layout: ", options->layout_id));
    }
    if (options->profile != MR_IAMF_AOM_PROFILE_AUTO && options->profile != MR_IAMF_AOM_PROFILE_SIMPLE) {
        return fail(
            error_buffer, error_buffer_size, "this bridge build supports only auto/simple channel-based output");
    }

    const std::filesystem::path input_path(options->input_wav_path);
    const std::filesystem::path output_path(options->output_iamf_path);
    const auto input_dir = input_path.has_parent_path() ? input_path.parent_path().string() : std::string(".");
    const auto output_dir = output_path.has_parent_path() ? output_path.parent_path().string() : std::string(".");
    const auto prefix = output_path.stem().string();

    iamf_tools_cli_proto::UserMetadata metadata;
    const std::string text = build_metadata_text(*options, *layout, input_path.filename().string(), prefix);
    if (!google::protobuf::TextFormat::ParseFromString(text, &metadata)) {
        return fail(error_buffer, error_buffer_size, "failed to build IAMF user metadata");
    }

    const absl::Status status = iamf_tools::TestMain(metadata, input_dir, output_dir);
    if (!status.ok()) {
        return fail(error_buffer, error_buffer_size, status.ToString());
    }

    for (int i = 0; i < 8; ++i) {
        std::filesystem::remove(std::filesystem::path(output_dir) /
                                absl::StrCat(prefix, "_rendered_id_42_sub_mix_0_layout_", i, ".wav"));
    }
    return 0;
}
