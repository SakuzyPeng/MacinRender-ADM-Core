#include "metadata.h"

#include <array>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>

#include "adm_ffi.h"

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
using Handle = std::unique_ptr<MradmAdmHandle, decltype(&mradm_adm_destroy)>;
using Buffer = std::unique_ptr<MradmAdmBuffer, decltype(&mradm_adm_buffer_destroy)>;
struct Parsed {
    // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes): private adapter aggregate owns its borrowed view.
    Handle handle{nullptr, mradm_adm_destroy};
    // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes): view lifetime is bounded by the handle above.
    MradmAdmView view{};
    Parsed(std::string_view xml, const std::map<std::string, uint16_t>& channels, uint32_t rate) {
        std::vector<MradmAdmChna> chna;
        chna.reserve(channels.size());
        for (const auto& [uid, channel] : channels) {
            chna.push_back({text(uid), channel});
        }
        std::array<uint8_t, 1024> error{};
        MradmAdmHandle* raw = nullptr;
        const auto code = mradm_adm_parse(reinterpret_cast<const uint8_t*>(xml.data()),
                                          xml.size(),
                                          rate,
                                          chna.data(),
                                          chna.size(),
                                          &raw,
                                          error.data(),
                                          error.size());
        handle.reset(raw);
        checked(code, error);
        checked(mradm_adm_view(handle.get(), &view, error.data(), error.size()), error);
    }
    [[nodiscard]] std::string string(uint32_t id) const {
        require(id < view.strings_len, "无效 ADM 字符串索引");
        return copy(view.strings[id]);
    }
    [[nodiscard]] std::optional<std::string> optional_string(uint32_t id) const {
        return id == k_none ? std::nullopt : std::optional{string(id)};
    }
    template <class T> static std::span<const T> subset(const T* values, std::size_t count, MradmAdmSpan span) {
        require(span.offset <= count && span.len <= count - span.offset, "无效 ADM 记录范围");
        return {values + span.offset, span.len};
    }
    [[nodiscard]] std::vector<std::string> strings(MradmAdmSpan span) const {
        std::vector<std::string> output;
        for (auto id : subset(view.indices, view.indices_len, span)) {
            output.push_back(string(id));
        }
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
std::optional<SceneLoudnessMetadata> loudness(const Parsed& parsed, const MradmAdmLoudness& v) {
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
SceneDirectSpeakersBlock direct_block(const Parsed& p, const MradmAdmDirectBlock& v) {
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
    b.adm_source = block_source(v.source);
    if (b.has_position) {
        b.source_position = position(v.position);
    }
    return b;
}
// NOLINTNEXTLINE(readability-function-size): explicit field mapping keeps Rust types outside the public scene.
AdmScene scene(const Parsed& p) {
    AdmScene output;
    const auto& view = p.view;
    for (const auto& v : std::span{view.programmes, view.programmes_len}) {
        output.programmes.push_back({p.string(v.id),
                                     p.string(v.name),
                                     p.strings(v.content_ids),
                                     p.optional_string(v.language),
                                     p.strings(v.labels),
                                     v.start,
                                     optional(v.end_present, v.end),
                                     loudness(p, v.loudness),
                                     v.reference_screen != 0});
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
    for (const auto& v : std::span{view.objects, view.objects_len}) {
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
        o.adm_source = SceneObjectSource{gain_source(v.source_gain),
                                         v.mute_present != 0,
                                         v.mute != 0,
                                         v.start_present != 0,
                                         v.start,
                                         v.absolute_start,
                                         optional(v.duration_present, v.duration),
                                         p.strings(v.child_objects),
                                         v.has_parent != 0};
        for (const auto& t : Parsed::subset(view.tracks, view.tracks_len, v.tracks)) {
            SceneTrackRef track;
            track.track_uid = p.string(t.uid);
            if (t.channel >= 0) {
                track.channel_index = static_cast<uint16_t>(t.channel);
            }
            for (const auto& b : Parsed::subset(view.object_blocks, view.object_blocks_len, t.blocks)) {
                track.blocks.push_back(object_block(b));
            }
            for (const auto& b : Parsed::subset(view.direct_blocks, view.direct_blocks_len, t.ds_blocks)) {
                track.ds_blocks.push_back(direct_block(p, b));
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
        for (const auto& c : Parsed::subset(view.hoa_channels, view.hoa_channels_len, v.channels)) {
            SceneHOAChannel channel;
            channel.track_uid = p.string(c.uid);
            channel.order = c.order;
            channel.degree = c.degree;
            if (c.channel >= 0) {
                channel.channel_index = static_cast<uint16_t>(c.channel);
            }
            for (const auto& b : Parsed::subset(view.hoa_blocks, view.hoa_blocks_len, c.blocks)) {
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
struct Patches {
    std::vector<MradmAdmPatch> values;
    template <class T> void add(uint32_t node, uint32_t field, T baseline, T original, T effective) {
        require(baseline == original, "原场景与源 AXML 语义不一致");
        values.push_back({node, field, 1, 1, static_cast<double>(effective), static_cast<double>(original), 0, 0});
    }
    template <class T>
    void add_optional(uint32_t node,
                      uint32_t field,
                      std::optional<T> baseline,
                      std::optional<T> original,
                      std::optional<T> effective) {
        require(baseline == original, "原场景与源 AXML 可选字段不一致");
        MradmAdmPatch patch{node,
                            field,
                            static_cast<uint32_t>(effective.has_value()),
                            static_cast<uint32_t>(original.has_value()),
                            0,
                            0,
                            0,
                            0};
        if (field == 14) {
            patch.samples = static_cast<uint64_t>(effective.value_or(0));
            patch.original_samples = static_cast<uint64_t>(original.value_or(0));
        } else {
            patch.value = static_cast<double>(effective.value_or(0));
            patch.original = static_cast<double>(original.value_or(0));
        }
        values.push_back(patch);
    }
};
template <class T> void same_size(const T& a, const T& b, const T& c) {
    require(a.size() == b.size() && b.size() == c.size(), "ADM 场景结构不一致");
}
template <class T> void same_time(const T& baseline, const T& original, const T& effective) {
    require(baseline.start_sample == original.start_sample && original.start_sample == effective.start_sample &&
                baseline.end_sample == original.end_sample && original.end_sample == effective.end_sample,
            "ADM 时间线结构不一致");
}
// NOLINTNEXTLINE(readability-function-size): validate and map the complete supported semantic field set together.
Patches collect_patches(const Parsed& parsed, const AdmScene& original, const AdmScene& effective) {
    const auto baseline = scene(parsed);
    same_size(baseline.objects, original.objects, effective.objects);
    same_size(baseline.hoa_tracks, original.hoa_tracks, effective.hoa_tracks);
    Patches patches;
    for (std::size_t oi = 0; oi < baseline.objects.size(); ++oi) {
        const auto& b = baseline.objects[oi];
        const auto& o = original.objects[oi];
        const auto& e = effective.objects[oi];
        const auto& record = parsed.view.objects[oi];
        require(b.id == o.id && o.id == e.id && b.end_sample == o.end_sample && o.end_sample == e.end_sample,
                "ADM 对象身份或时间不一致");
        patches.add(record.node, 1, b.gain, o.gain, e.gain);
        patches.add(record.node, 2, b.mute, o.mute, e.mute);
        same_size(b.tracks, o.tracks, e.tracks);
        for (std::size_t ti = 0; ti < b.tracks.size(); ++ti) {
            const auto& bt = b.tracks[ti];
            const auto& ot = o.tracks[ti];
            const auto& et = e.tracks[ti];
            const auto& track = parsed.view.tracks[record.tracks.offset + ti];
            require(bt.track_uid == ot.track_uid && ot.track_uid == et.track_uid, "ADM 轨道顺序不一致");
            same_size(bt.blocks, ot.blocks, et.blocks);
            same_size(bt.ds_blocks, ot.ds_blocks, et.ds_blocks);
            for (std::size_t bi = 0; bi < bt.blocks.size(); ++bi) {
                const auto& bb = bt.blocks[bi];
                const auto& ob = ot.blocks[bi];
                const auto& eb = et.blocks[bi];
                same_time(bb, ob, eb);
                const auto node = parsed.view.object_blocks[track.blocks.offset + bi].node;
                patches.add(node, 1, bb.gain, ob.gain, eb.gain);
                patches.add(node, 3, bb.head_locked, ob.head_locked, eb.head_locked);
                patches.add(node, 4, bb.diffuse, ob.diffuse, eb.diffuse);
                patches.add(node, 5, bb.width, ob.width, eb.width);
                patches.add(node, 6, bb.height, ob.height, eb.height);
                patches.add(node, 7, bb.depth, ob.depth, eb.depth);
                patches.add(node, 8, bb.divergence, ob.divergence, eb.divergence);
                patches.add(
                    node, 9, bb.divergence_azimuth_range, ob.divergence_azimuth_range, eb.divergence_azimuth_range);
                patches.add(
                    node, 10, bb.divergence_position_range, ob.divergence_position_range, eb.divergence_position_range);
                patches.add(node, 11, bb.channel_lock, ob.channel_lock, eb.channel_lock);
                patches.add_optional(
                    node, 12, bb.channel_lock_max_distance, ob.channel_lock_max_distance, eb.channel_lock_max_distance);
                patches.add(node, 13, bb.jump_position, ob.jump_position, eb.jump_position);
                patches.add_optional(
                    node, 14, bb.interp_length_samples, ob.interp_length_samples, eb.interp_length_samples);
            }
            for (std::size_t bi = 0; bi < bt.ds_blocks.size(); ++bi) {
                const auto& bb = bt.ds_blocks[bi];
                const auto& ob = ot.ds_blocks[bi];
                const auto& eb = et.ds_blocks[bi];
                same_time(bb, ob, eb);
                const auto node = parsed.view.direct_blocks[track.ds_blocks.offset + bi].node;
                patches.add(node, 1, bb.gain, ob.gain, eb.gain);
                patches.add(node, 3, bb.head_locked, ob.head_locked, eb.head_locked);
            }
        }
    }
    for (std::size_t pi = 0; pi < baseline.hoa_tracks.size(); ++pi) {
        const auto& b = baseline.hoa_tracks[pi];
        const auto& o = original.hoa_tracks[pi];
        const auto& e = effective.hoa_tracks[pi];
        const auto& record = parsed.view.hoa[pi];
        require(b.object_id == o.object_id && o.object_id == e.object_id && b.pack_format_id == o.pack_format_id &&
                    o.pack_format_id == e.pack_format_id,
                "HOA pack 身份不一致");
        same_size(b.channels, o.channels, e.channels);
        for (std::size_t ci = 0; ci < b.channels.size(); ++ci) {
            const auto& bc = b.channels[ci];
            const auto& oc = o.channels[ci];
            const auto& ec = e.channels[ci];
            const auto& channel = parsed.view.hoa_channels[record.channels.offset + ci];
            require(bc.track_uid == oc.track_uid && oc.track_uid == ec.track_uid, "HOA 轨道顺序不一致");
            same_size(bc.blocks, oc.blocks, ec.blocks);
            for (std::size_t bi = 0; bi < bc.blocks.size(); ++bi) {
                const auto& bb = bc.blocks[bi];
                const auto& ob = oc.blocks[bi];
                const auto& eb = ec.blocks[bi];
                same_time(bb, ob, eb);
                patches.add(parsed.view.hoa_blocks[channel.blocks.offset + bi].node,
                            3,
                            bb.head_locked,
                            ob.head_locked,
                            eb.head_locked);
            }
        }
    }
    return patches;
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
Result<AdmScene> import_axml(std::string_view xml, const std::map<std::string, uint16_t>& channels, uint32_t rate) {
    try {
        return scene(Parsed{xml, channels, rate});
    } catch (const Failure& e) {
        return make_error(e.code(), e.what());
    }
}
Result<std::string> patch_axml(std::string_view xml, const AdmScene& original, const AdmScene& effective) {
    try {
        require(original.info.sample_rate == effective.info.sample_rate, "ADM 采样率不一致");
        Parsed parsed{xml, {}, original.info.sample_rate};
        const auto patches = collect_patches(parsed, original, effective);
        std::array<uint8_t, 1024> error{};
        MradmAdmBuffer* raw = nullptr;
        const auto code = mradm_adm_patch(
            parsed.handle.get(), patches.values.data(), patches.values.size(), &raw, error.data(), error.size());
        Buffer buffer{raw, mradm_adm_buffer_destroy};
        checked(code, error);
        return output(std::move(buffer)).axml;
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
