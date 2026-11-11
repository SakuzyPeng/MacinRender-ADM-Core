#include "metadata.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>

#include "scene_ffi.h"

namespace mradm::metadata {
namespace {
constexpr auto k_none = std::numeric_limits<uint32_t>::max();
class Failure : public std::runtime_error {
  public:
    Failure(ErrorCode error_code, const std::string& message) : std::runtime_error(message), code_(error_code) {}
    [[nodiscard]] ErrorCode code() const { return code_; }

  private:
    ErrorCode code_;
};
void require(bool condition, std::string_view message) {
    if (!condition) {
        throw Failure(ErrorCode::invalid_argument, std::string{message});
    }
}
void checked(int32_t code, const std::array<uint8_t, 1024>& error) {
    if (code != 0) {
        throw Failure(static_cast<ErrorCode>(code), reinterpret_cast<const char*>(error.data()));
    }
}
MradmAdmText text(std::string_view value) {
    return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}
std::string copy(MradmAdmText value) {
    return value.len == 0 ? std::string{} : std::string{reinterpret_cast<const char*>(value.data), value.len};
}
using Buffer = std::unique_ptr<MradmAdmBuffer, decltype(&mradm_adm_buffer_destroy)>;
// Bounds-checked reads of a borrowed record view (an ADM document or an imported render scene). The
// view is a plain set of pointers; the records stay owned by its handle.
struct Records {
    // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes): private adapter aggregate over a borrowed view.
    MradmAdmView view;
    [[nodiscard]] std::string string(uint32_t id) const {
        require(id < view.strings_len, "无效 ADM 字符串索引");
        return copy(view.strings[id]);
    }
    [[nodiscard]] std::optional<std::string> optional_string(uint32_t id) const {
        return id == k_none ? std::nullopt : std::optional{string(id)};
    }
    template <class T> static std::span<const T> subset(const T* values, std::size_t count, MradmAdmSpan span) {
        require(span.offset <= count && span.len <= count - span.offset, "无效 ADM 记录范围");
        if (span.len == 0) {
            return {};
        }
        return {values + span.offset, span.len};
    }
    [[nodiscard]] std::vector<std::string> strings(MradmAdmSpan span) const {
        std::vector<std::string> output;
        output.reserve(span.len);
        std::ranges::transform(subset(view.indices, view.indices_len, span),
                               std::back_inserter(output),
                               [this](uint32_t id) { return string(id); });
        return output;
    }
};
template <class T> std::optional<T> optional(uint32_t present, T value) {
    return present != 0 ? std::optional<T>{value} : std::nullopt;
}
SceneBlockPosition position(const MradmAdmPosition& p) {
    return {p.cartesian != 0, p.azimuth, p.elevation, p.distance, p.x, p.y, p.z};
}
SceneGainSource gain_source(const MradmAdmGain& gain) {
    return {gain.present != 0, gain.decibels != 0, gain.value, gain.linear};
}
SceneBlockSource block_source(const MradmAdmBlockSource& source) {
    return {gain_source(source.gain),
            source.rtime_present != 0,
            source.rtime_samples,
            optional(source.duration_present, source.duration_samples)};
}
std::optional<SceneLoudnessMetadata> loudness(const Records& parsed, const MradmAdmLoudness& v) {
    if (v.present == 0) {
        return std::nullopt;
    }
    return SceneLoudnessMetadata{optional(v.fields & 1U, v.integrated),
                                 optional(v.fields & 2U, v.true_peak),
                                 optional(v.fields & 4U, v.range),
                                 optional(v.fields & 8U, v.momentary),
                                 optional(v.fields & 16U, v.short_term),
                                 optional(v.fields & 32U, v.dialogue),
                                 parsed.optional_string(v.method)};
}
SceneObjectBlock object_block(const MradmAdmObjectBlock& v) {
    SceneObjectBlock b;
    b.position = position(v.position);
    b.gain = v.gain;
    b.diffuse = v.diffuse;
    b.width = v.width;
    b.height = v.height;
    b.depth = v.depth;
    b.start_sample = v.start;
    b.end_sample = v.end;
    b.jump_position = v.jump != 0;
    b.interp_length_samples = optional(v.interpolation_present, v.interpolation);
    b.channel_lock = v.channel_lock != 0;
    b.channel_lock_max_distance = optional(v.max_distance_present, v.max_distance);
    b.divergence = v.divergence;
    b.divergence_azimuth_range = v.azimuth_range;
    b.divergence_position_range = v.position_range;
    b.screen_ref = v.screen_ref != 0;
    b.head_locked = v.head_locked != 0;
    b.adm_source = block_source(v.source);
    return b;
}
SceneUserLevel user_level(float gain, uint32_t mute) {
    return {gain, mute == 0U ? std::nullopt : std::optional{mute == 2U}};
}
// A semantic policy's state beside the records (PCM consistency 4.4): the record keeps the authored
// has_position, which gates the source position, and the effective one comes from the overlay.
SceneDirectSpeakersBlock
direct_block(const Records& p, const MradmAdmDirectBlock& v, bool provenance, const MradmSceneDirectState* state) {
    SceneDirectSpeakersBlock b;
    b.speaker_labels = p.strings(v.labels);
    b.pack_format_id = p.string(v.pack_id);
    b.azimuth = v.azimuth;
    b.elevation = v.elevation;
    b.distance = v.distance;
    b.has_position = v.has_position != 0;
    b.azimuth_min = optional(v.bounds_present & 1U, v.azimuth_min);
    b.azimuth_max = optional(v.bounds_present & 2U, v.azimuth_max);
    b.elevation_min = optional(v.bounds_present & 4U, v.elevation_min);
    b.elevation_max = optional(v.bounds_present & 8U, v.elevation_max);
    b.distance_min = optional(v.bounds_present & 16U, v.distance_min);
    b.distance_max = optional(v.bounds_present & 32U, v.distance_max);
    b.gain = v.gain;
    b.low_pass_hz = optional(v.low_pass_present, v.low_pass);
    b.start_sample = v.start;
    b.end_sample = v.end;
    b.head_locked = v.head_locked != 0;
    if (provenance) {
        b.adm_source = block_source(v.source);
        if (v.has_position != 0) {
            b.source_position = position(v.position);
        }
    }
    if (state != nullptr) {
        b.has_position = state->has_position != 0;
        b.user_level = user_level(state->user_gain, state->user_mute);
        b.user_position_override = state->position_override != 0;
    }
    return b;
}
// A synthesized channel bed has no ADM provenance: its source records are not read.
// NOLINTNEXTLINE(readability-function-size): explicit field mapping keeps Rust types outside the public scene.
AdmScene scene(const Records& p, bool provenance, const MradmSceneOverlay* overlay = nullptr) {
    AdmScene output;
    const auto& view = p.view;
    require(overlay == nullptr ||
                (overlay->objects_len == view.objects_len && overlay->direct_blocks_len == view.direct_blocks_len),
            "无效场景 overlay");
    for (const auto& v : std::span{view.programmes, view.programmes_len}) {
        output.programmes.push_back(
            {p.string(v.id),
             p.string(v.name),
             p.strings(v.content_ids),
             p.optional_string(v.language),
             p.strings(v.labels),
             v.start,
             optional(v.end_present, v.end),
             loudness(p, v.loudness),
             v.reference_screen != 0,
             v.reference_screen != 0
                 ? std::optional{ScreenGeometry{
                       v.screen_geometry.cartesian != 0,
                       v.screen_geometry.aspect_ratio,
                       {v.screen_geometry.centre[0], v.screen_geometry.centre[1], v.screen_geometry.centre[2]},
                       v.screen_geometry.width}}
                 : std::nullopt});
    }
    for (const auto& v : std::span{view.contents, view.contents_len}) {
        output.contents.push_back({p.string(v.id),
                                   p.string(v.name),
                                   p.strings(v.object_ids),
                                   p.optional_string(v.language),
                                   p.strings(v.labels),
                                   loudness(p, v.loudness),
                                   p.optional_string(v.dialogue_kind),
                                   p.optional_string(v.content_kind)});
    }
    for (std::size_t oi = 0; oi < view.objects_len; ++oi) {
        const auto& v = view.objects[oi];
        SceneObject o;
        o.id = p.string(v.id);
        o.name = p.string(v.name);
        o.gain = v.gain;
        o.mute = v.mute != 0;
        o.head_locked = v.head_locked != 0;
        o.end_sample = v.end;
        if (v.offset_present != 0) {
            const auto& q = v.position_offset;
            o.position_offset =
                ScenePositionOffset{q.cartesian != 0, q.azimuth, q.elevation, q.distance, q.x, q.y, q.z};
        }
        o.labels = p.strings(v.labels);
        o.importance = optional(v.importance_present, static_cast<int>(v.importance));
        o.dialogue_id = optional(v.dialogue_present, static_cast<unsigned int>(v.dialogue));
        if (overlay != nullptr) {
            const auto& state = overlay->objects[oi];
            o.mute = state.mute != 0;
            o.user_level = user_level(state.user_gain, state.user_mute);
        }
        if (provenance) {
            o.adm_source = SceneObjectSource{gain_source(v.source_gain),
                                             v.mute_present != 0,
                                             v.mute != 0,
                                             v.start_present != 0,
                                             v.start,
                                             v.absolute_start,
                                             optional(v.duration_present, v.duration),
                                             p.strings(v.child_objects),
                                             v.has_parent != 0};
        }
        for (const auto& t : Records::subset(view.tracks, view.tracks_len, v.tracks)) {
            SceneTrackRef track;
            track.track_uid = p.string(t.uid);
            if (t.channel >= 0) {
                track.channel_index = static_cast<uint16_t>(t.channel);
            }
            for (const auto& b : Records::subset(view.object_blocks, view.object_blocks_len, t.blocks)) {
                track.blocks.push_back(object_block(b));
            }
            const auto ds_blocks = Records::subset(view.direct_blocks, view.direct_blocks_len, t.ds_blocks);
            for (std::size_t bi = 0; bi < ds_blocks.size(); ++bi) {
                const auto* state = overlay != nullptr ? &overlay->direct_blocks[t.ds_blocks.offset + bi] : nullptr;
                track.ds_blocks.push_back(direct_block(p, ds_blocks[bi], provenance, state));
            }
            o.tracks.push_back(std::move(track));
        }
        output.objects.push_back(std::move(o));
    }
    for (const auto& v : std::span{view.hoa, view.hoa_len}) {
        SceneHOATracks pack;
        pack.object_id = p.string(v.object_id);
        pack.pack_format_id = p.string(v.pack_id);
        pack.normalization = p.string(v.normalization);
        pack.nfc_ref_dist = v.nfc_ref_dist;
        pack.screen_ref = v.screen_ref != 0;
        pack.gain = v.gain;
        pack.mute = v.mute != 0;
        pack.head_locked = v.head_locked != 0;
        pack.start_sample = v.start;
        pack.end_sample = v.end;
        for (const auto& c : Records::subset(view.hoa_channels, view.hoa_channels_len, v.channels)) {
            SceneHOAChannel channel;
            channel.track_uid = p.string(c.uid);
            channel.order = c.order;
            channel.degree = c.degree;
            if (c.channel >= 0) {
                channel.channel_index = static_cast<uint16_t>(c.channel);
            }
            for (const auto& b : Records::subset(view.hoa_blocks, view.hoa_blocks_len, c.blocks)) {
                channel.blocks.push_back({b.gain, b.start, b.end, b.head_locked != 0});
            }
            pack.channels.push_back(std::move(channel));
        }
        output.hoa_tracks.push_back(std::move(pack));
    }
    for (auto warning : std::span{view.warnings, view.warnings_len}) {
        output.import_warnings.push_back(p.string(warning));
    }
    return output;
}
OutputMetadata output(Buffer buffer) {
    std::array<uint8_t, 1024> error{};
    MradmAdmText xml{};
    const MradmAdmOutputChna* entries = nullptr;
    std::size_t count = 0;
    checked(mradm_adm_buffer_view(buffer.get(), &xml, &entries, &count, error.data(), error.size()), error);
    OutputMetadata result{copy(xml), {}};
    for (const auto& entry : std::span{entries, count}) {
        result.chna.push_back({entry.track, copy(entry.uid), copy(entry.format), copy(entry.pack)});
    }
    return result;
}
} // namespace
Result<AdmScene> scene_from_view(const MradmAdmView& view, bool adm_provenance, const MradmSceneOverlay* overlay) {
    try {
        return scene(Records{view}, adm_provenance, overlay);
    } catch (const Failure& e) {
        return make_error(e.code(), e.what());
    }
}
Result<OutputMetadata>
generate(uint32_t kind, std::string_view name, const std::vector<render_layouts::SpeakerSpec>& speakers) {
    try {
        std::vector<MradmAdmSpeaker> records;
        std::vector<MradmAdmText> labels;
        for (const auto& speaker : speakers) {
            MradmAdmSpeaker s{};
            s.label = static_cast<uint32_t>(labels.size());
            s.azimuth = speaker.azimuth;
            s.elevation = speaker.elevation;
            s.lfe = static_cast<uint32_t>(speaker.is_lfe);
            if (speaker.azimuth_range) {
                s.ranges |= 1U;
                s.azimuth_min = speaker.azimuth_range->first;
                s.azimuth_max = speaker.azimuth_range->second;
            }
            if (speaker.elevation_range) {
                s.ranges |= 2U;
                s.elevation_min = speaker.elevation_range->first;
                s.elevation_max = speaker.elevation_range->second;
            }
            records.push_back(s);
            labels.push_back(text(speaker.label));
        }
        std::array<uint8_t, 1024> error{};
        MradmAdmBuffer* raw = nullptr;
        const auto code = mradm_adm_generate(
            kind, text(name), records.data(), labels.data(), records.size(), &raw, error.data(), error.size());
        Buffer buffer{raw, mradm_adm_buffer_destroy};
        checked(code, error);
        return output(std::move(buffer));
    } catch (const Failure& e) {
        return make_error(e.code(), e.what());
    }
}
} // namespace mradm::metadata
