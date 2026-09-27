#include "room_compat_bed.h"

#include <array>
#include <cmath>
#include <set>
#include <string>

#include "room_222.h"

namespace mradm::room_compat {
namespace {
constexpr std::array<std::string_view, 10> k_labels{
    "RC_L", "RC_R", "RC_C", "RC_LFE", "RC_Lss", "RC_Rss", "RC_Lrs", "RC_Rrs", "RC_Lts", "RC_Rts"};
}

std::optional<std::size_t> bed_channel(const SceneDirectSpeakersBlock& block) {
    if (block.speaker_labels.size() == 1) {
        for (std::size_t index = 0; index < k_labels.size(); ++index) {
            if (block.speaker_labels.front() == k_labels.at(index)) {
                return index;
            }
        }
    }
    return std::nullopt;
}

float bed_user_gain(const SceneDirectSpeakersBlock& block) {
    return block.user_level.mute.value_or(false) ? 0.0F : block.user_level.gain_multiplier;
}

Result<void> validate_bed(const SceneObject& object, const SceneInfo& info) {
    if (info.sample_rate != 48000 || object.tracks.size() != 10) {
        return make_error(ErrorCode::unsupported,
                          "room-compat bed requires one complete 48 kHz 7.1.2 bed",
                          "object=" + object.id + "; field=bed_format/sample_rate");
    }
    std::set<std::size_t> channels;
    std::optional<std::string> pack;
    for (const auto& track : object.tracks) {
        const auto context = "object=" + object.id + "; track=" + track.track_uid;
        if (!track.blocks.empty() || track.ds_blocks.empty() || !track.channel_index) {
            return make_error(ErrorCode::unsupported,
                              "room-compat bed requires independent DirectSpeakers tracks",
                              context + "; field=track_binding");
        }
        const auto index = bed_channel(track.ds_blocks.front());
        if (!index || *track.channel_index != *index || !channels.insert(*index).second) {
            return make_error(ErrorCode::unsupported,
                              "room-compat requires the verified ordered RC 7.1.2 bed labels",
                              context + "; field=speakerLabel/PCM");
        }
        if (!pack) {
            pack = track.ds_blocks.front().pack_format_id;
        }
        const auto& level = track.ds_blocks.front().user_level;
        for (const auto& block : track.ds_blocks) {
            const auto at = context + "; sample=" + std::to_string(block.start_sample);
            if (bed_channel(block) != index || block.pack_format_id != *pack) {
                return make_error(ErrorCode::unsupported,
                                  "room-compat bed labels and pack must remain constant",
                                  at + "; field=speakerLabel/pack");
            }
            if (block.head_locked || block.low_pass_hz || block.azimuth_min || block.azimuth_max ||
                block.elevation_min || block.elevation_max || block.distance_min || block.distance_max ||
                block.user_position_override || (block.source_position && !block.source_position->cartesian)) {
                return make_error(ErrorCode::unsupported,
                                  "room-compat bed has unverified position/frequency modifiers",
                                  at + "; field=position/modifiers");
            }
            const float native_gain = block.adm_source ? block.adm_source->gain.linear : block.gain;
            if (!std::isfinite(native_gain) || native_gain < 0 || !std::isfinite(block.user_level.gain_multiplier) ||
                block.user_level.gain_multiplier < 0 || level.gain_multiplier != block.user_level.gain_multiplier ||
                level.mute != block.user_level.mute) {
                return make_error(
                    ErrorCode::unsupported, "room-compat bed requires finite static user gain", at + "; field=gain");
            }
        }
    }
    return {};
}

Result<std::vector<float>>
bed_gains(const SceneDirectSpeakersBlock& block, std::string_view layout, LfeRoutingMode lfe_mode) {
    const bool seven = layout == "4+7+0" || layout == "7.1.4";
    const auto index = bed_channel(block);
    if (!index || (!seven && layout != "9.1.6" && !is_room_222(layout))) {
        return make_error(ErrorCode::unsupported, "room bed requires verified labels and 7.1.4/9.1.6/22.2 output");
    }
    if (is_room_222(layout)) {
        constexpr std::array<std::size_t, 10> k_destinations{6, 7, 2, 3, 10, 11, 4, 5, 18, 19};
        std::vector<float> gains(k_room_222_channels, 0);
        if (*index == 3 && lfe_mode == LfeRoutingMode::split_power) {
            gains[3] = gains[9] = std::sqrt(.5F);
        } else {
            gains[k_destinations.at(*index)] = 1;
        }
        return gains;
    }
    std::vector<float> gains(seven ? 12U : 16U, 0.0F);
    if (*index < 8) {
        gains[*index] = 1.0F;
    } else if (seven) {
        const float equal_power = std::sqrt(0.5F);
        gains[*index] = equal_power;
        gains[*index + 2] = equal_power;
    } else {
        gains[*index + 4] = 1.0F;
    }
    return gains;
}
} // namespace mradm::room_compat
