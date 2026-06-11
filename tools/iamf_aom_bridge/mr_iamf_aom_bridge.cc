#include "mr_iamf_aom_bridge.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <stddef.h>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "google/protobuf/text_format.h"
#include "iamf/cli/mr_bridge/mr_iamf_aom_cancellable_encoder.h"
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

std::string_view canonical_layout_id(std::string_view id) {
    if (id == "stereo" || id == "2.0") {
        return "0+2+0";
    }
    if (id == "5.1") {
        return "0+5+0";
    }
    if (id == "5.1.2") {
        return "2+5+0";
    }
    if (id == "5.1.4") {
        return "4+5+0";
    }
    if (id == "7.1" || id == "0+7+0" || id == "wave_7_1" || id == "wave-7.1") {
        return "wav71";
    }
    if (id == "7.1.4") {
        return "4+7+0";
    }
    return id;
}

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
    const std::string_view canonical = canonical_layout_id(id);
    const auto it = std::ranges::find_if(k_layouts, [&](const LayoutInfo& layout) { return layout.id == canonical; });
    return it == k_layouts.end() ? nullptr : &*it;
}

std::string q78_field(const char* name, bool has_value, double value) {
    if (!has_value || !std::isfinite(value)) {
        return {};
    }
    return absl::StrCat(name, ": ", static_cast<int32_t>(std::lrint(value * 256.0)), "\n");
}

std::string build_metadata_text(const MrIamfAomEncodeOptions& options,
                                const LayoutInfo& final_layout,
                                const std::vector<const LayoutInfo*>& layers,
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
    for (uint32_t sid = 0; sid < final_layout.substream_count; ++sid) {
        out << "  audio_substream_ids: " << sid << "\n";
    }
    out << "  scalable_channel_layout_config { reserved: 0";
    for (const LayoutInfo* layer : layers) {
        out << " channel_audio_layer_configs { loudspeaker_layout: " << layer->loudspeaker_layout
            << " output_gain_is_present_flag: 0 recon_gain_is_present_flag: 0 reserved_a: 0 "
            << "substream_count: " << layer->substream_count
            << " coupled_substream_count: " << layer->coupled_substream_count;
        if (!layer->expanded_loudspeaker_layout.empty()) {
            out << " expanded_loudspeaker_layout: " << layer->expanded_loudspeaker_layout;
        }
        out << " }";
    }
    out << " }\n"
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
    std::vector<std::string_view> emitted_sound_systems{"SOUND_SYSTEM_A_0_2_0"};
    for (const LayoutInfo* layer : layers) {
        if (std::ranges::find(emitted_sound_systems, layer->sound_system) != emitted_sound_systems.end()) {
            continue;
        }
        emitted_sound_systems.push_back(layer->sound_system);
        out << "    layouts { loudness_layout { layout_type: LAYOUT_TYPE_LOUDSPEAKERS_SS_CONVENTION ss_layout { "
               "sound_system: "
            << layer->sound_system << " reserved: 0 } } loudness { info_type_bit_masks: [] "
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
    for (size_t i = 0; i < final_layout.channel_labels.size(); ++i) {
        out << "  channel_metadatas { channel_id: " << i << " channel_label: " << final_layout.channel_labels[i]
            << " }\n";
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
    return MR_IAMF_AOM_RESULT_ERROR;
}

bool has_v2_cancel_fields(const MrIamfAomEncodeOptions& options) {
    return options.abi_version >= 2 &&
           options.struct_size >= offsetof(MrIamfAomEncodeOptions, cancel_user_data) + sizeof(options.cancel_user_data);
}

bool has_v3_scalable_layers_field(const MrIamfAomEncodeOptions& options) {
    return options.abi_version >= 3 && options.struct_size >= offsetof(MrIamfAomEncodeOptions, scalable_layers_csv) +
                                                                  sizeof(options.scalable_layers_csv);
}

std::string_view trim_ascii(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

std::vector<const LayoutInfo*> parse_scalable_layers(const MrIamfAomEncodeOptions& options,
                                                     const LayoutInfo& final_layout,
                                                     char* error_buffer,
                                                     size_t error_buffer_size) {
    if (!has_v3_scalable_layers_field(options) || options.scalable_layers_csv == nullptr ||
        options.scalable_layers_csv[0] == '\0') {
        return {&final_layout};
    }

    std::vector<const LayoutInfo*> layers;
    std::string_view csv{options.scalable_layers_csv};
    while (true) {
        const size_t comma = csv.find(',');
        const std::string_view token = trim_ascii(csv.substr(0, comma));
        if (!token.empty()) {
            const LayoutInfo* layer = find_layout(token);
            if (layer == nullptr) {
                write_error(error_buffer, error_buffer_size, absl::StrCat("unsupported IAMF scalable layer: ", token));
                return {};
            }
            layers.push_back(layer);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        csv.remove_prefix(comma + 1);
    }

    if (layers.empty() || layers.size() > 7U) {
        write_error(error_buffer, error_buffer_size, "IAMF scalable layer count must be 1-7");
        return {};
    }
    if (layers.back()->id != final_layout.id) {
        write_error(error_buffer,
                    error_buffer_size,
                    absl::StrCat("last IAMF scalable layer must match final layout: ", final_layout.id));
        return {};
    }
    return layers;
}

bool should_cancel(const MrIamfAomEncodeOptions& options) {
    return has_v2_cancel_fields(options) && options.should_cancel != nullptr &&
           options.should_cancel(options.cancel_user_data) != 0;
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
    if (options->abi_version < 1 || options->abi_version > MR_IAMF_AOM_BRIDGE_ABI_VERSION) {
        return fail(error_buffer, error_buffer_size, "bridge ABI version mismatch");
    }
    if (should_cancel(*options)) {
        write_error(error_buffer, error_buffer_size, "IAMF encode cancelled");
        return MR_IAMF_AOM_RESULT_CANCELLED;
    }
    if (options->input_wav_path == nullptr || options->output_iamf_path == nullptr || options->layout_id == nullptr) {
        return fail(error_buffer, error_buffer_size, "input_wav_path, output_iamf_path, and layout_id are required");
    }

    const LayoutInfo* layout = find_layout(options->layout_id);
    if (layout == nullptr) {
        return fail(error_buffer, error_buffer_size, absl::StrCat("unsupported IAMF layout: ", options->layout_id));
    }
    const auto layers = parse_scalable_layers(*options, *layout, error_buffer, error_buffer_size);
    if (layers.empty()) {
        return MR_IAMF_AOM_RESULT_ERROR;
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
    const std::string text = build_metadata_text(*options, *layout, layers, input_path.filename().string(), prefix);
    if (!google::protobuf::TextFormat::ParseFromString(text, &metadata)) {
        return fail(error_buffer, error_buffer_size, "failed to build IAMF user metadata");
    }

    const absl::Status status = iamf_tools::TestMainCancellable(
        metadata, input_dir, output_dir, [&options] { return should_cancel(*options); });
    if (status.code() == absl::StatusCode::kCancelled) {
        write_error(error_buffer, error_buffer_size, status.ToString());
        return MR_IAMF_AOM_RESULT_CANCELLED;
    }
    if (!status.ok()) {
        return fail(error_buffer, error_buffer_size, status.ToString());
    }

    for (int i = 0; i < 16; ++i) {
        std::filesystem::remove(std::filesystem::path(output_dir) /
                                absl::StrCat(prefix, "_rendered_id_42_sub_mix_0_layout_", i, ".wav"));
    }
    return MR_IAMF_AOM_RESULT_OK;
}
