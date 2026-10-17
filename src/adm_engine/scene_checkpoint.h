#pragma once

// Diagnostic-only canonical word stream of an AdmScene (PCM consistency 4.1). RenderService
// records it after import and after the semantic policy so the Rust render scene (phase 4.3/4.4)
// can be compared word for word. The encoding is specified in
// docs/architecture/RUST_INVERSION_BASELINE.md; environment-dependent content (the input path,
// warning text) is excluded. Temporary: removed once Rust emits the same checkpoints.
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
#include <bit>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "adm/scene.h"

#include "consistency_trace.h"

namespace mradm::scene_checkpoint {

class Words {
  public:
    void u32(std::uint32_t v) { words_.push_back(static_cast<int>(v)); }
    void u64(std::uint64_t v) {
        u32(static_cast<std::uint32_t>(v));
        u32(static_cast<std::uint32_t>(v >> 32U));
    }
    void boolean(bool v) { u32(v ? 1U : 0U); }
    void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }
    void f64(double v) { u64(std::bit_cast<std::uint64_t>(v)); }
    void text(std::string_view v) {
        u32(static_cast<std::uint32_t>(v.size()));
        for (std::size_t i = 0; i < v.size(); i += 4) {
            std::uint32_t word = 0;
            for (std::size_t j = 0; j < 4 && i + j < v.size(); ++j) {
                word |= static_cast<std::uint32_t>(static_cast<unsigned char>(v[i + j])) << (8U * j);
            }
            u32(word);
        }
    }
    template <class T, class F> void optional(const std::optional<T>& v, const F& write) {
        boolean(v.has_value());
        if (v) {
            write(*v);
        }
    }
    template <class T, class F> void list(const std::vector<T>& v, const F& write) {
        u32(static_cast<std::uint32_t>(v.size()));
        for (const auto& item : v) {
            write(item);
        }
    }
    void texts(const std::vector<std::string>& v) {
        list(v, [this](const std::string& s) { text(s); });
    }
    [[nodiscard]] const std::vector<int>& words() const { return words_; }

  private:
    std::vector<int> words_;
};

inline void position(Words& w, const SceneBlockPosition& p) {
    w.boolean(p.cartesian);
    for (float v : {p.azimuth, p.elevation, p.distance, p.x, p.y, p.z}) {
        w.f32(v);
    }
}

inline void gain_source(Words& w, const SceneGainSource& g) {
    w.boolean(g.present);
    w.boolean(g.decibels);
    w.f64(g.value);
    w.f32(g.linear);
}

inline void block_source(Words& w, const SceneBlockSource& s) {
    gain_source(w, s.gain);
    w.boolean(s.rtime_present);
    w.u64(s.rtime_samples);
    w.optional(s.duration_samples, [&w](std::uint64_t v) { w.u64(v); });
}

inline void object_block(Words& w, const SceneObjectBlock& b) {
    position(w, b.position);
    for (float v : {b.gain, b.diffuse, b.width, b.height, b.depth}) {
        w.f32(v);
    }
    w.u64(b.start_sample);
    w.u64(b.end_sample);
    w.boolean(b.jump_position);
    w.optional(b.interp_length_samples, [&w](std::uint64_t v) { w.u64(v); });
    w.boolean(b.channel_lock);
    w.optional(b.channel_lock_max_distance, [&w](float v) { w.f32(v); });
    w.f32(b.divergence);
    w.f32(b.divergence_azimuth_range);
    w.f32(b.divergence_position_range);
    w.boolean(b.screen_ref);
    w.boolean(b.head_locked);
    w.optional(b.adm_source, [&w](const SceneBlockSource& s) { block_source(w, s); });
}

inline void direct_block(Words& w, const SceneDirectSpeakersBlock& b) {
    w.texts(b.speaker_labels);
    w.text(b.pack_format_id);
    w.f32(b.azimuth);
    w.f32(b.elevation);
    w.f32(b.distance);
    w.boolean(b.has_position);
    for (const auto* v :
         {&b.azimuth_min, &b.azimuth_max, &b.elevation_min, &b.elevation_max, &b.distance_min, &b.distance_max}) {
        w.optional(*v, [&w](float x) { w.f32(x); });
    }
    w.f32(b.gain);
    w.optional(b.low_pass_hz, [&w](float v) { w.f32(v); });
    w.u64(b.start_sample);
    w.u64(b.end_sample);
    w.boolean(b.head_locked);
    w.optional(b.adm_source, [&w](const SceneBlockSource& s) { block_source(w, s); });
    w.optional(b.source_position, [&w](const SceneBlockPosition& p) { position(w, p); });
    w.f32(b.user_level.gain_multiplier);
    w.optional(b.user_level.mute, [&w](bool v) { w.boolean(v); });
    w.boolean(b.user_position_override);
}

inline void loudness(Words& w, const SceneLoudnessMetadata& l) {
    for (const auto* v : {&l.integrated_loudness,
                          &l.max_true_peak,
                          &l.loudness_range,
                          &l.max_momentary,
                          &l.max_short_term,
                          &l.dialogue_loudness}) {
        w.optional(*v, [&w](float x) { w.f32(x); });
    }
    w.optional(l.loudness_method, [&w](const std::string& s) { w.text(s); });
}

inline void screen(Words& w, const ScreenGeometry& s) {
    w.boolean(s.cartesian);
    w.f64(s.aspect_ratio);
    for (double v : s.centre) {
        w.f64(v);
    }
    w.f64(s.width);
}

inline void object(Words& w, const SceneObject& o) {
    w.text(o.id);
    w.text(o.name);
    w.f32(o.gain);
    w.boolean(o.mute);
    w.u64(o.end_sample);
    w.optional(o.position_offset, [&w](const ScenePositionOffset& p) {
        w.boolean(p.cartesian);
        for (float v : {p.azimuth, p.elevation, p.distance, p.x, p.y, p.z}) {
            w.f32(v);
        }
    });
    w.list(o.tracks, [&w](const SceneTrackRef& t) {
        w.optional(t.channel_index, [&w](std::uint16_t v) { w.u32(v); });
        w.text(t.track_uid);
        w.list(t.blocks, [&w](const SceneObjectBlock& b) { object_block(w, b); });
        w.list(t.ds_blocks, [&w](const SceneDirectSpeakersBlock& b) { direct_block(w, b); });
    });
    w.texts(o.labels);
    w.optional(o.importance, [&w](int v) { w.u32(static_cast<std::uint32_t>(v)); });
    w.optional(o.dialogue_id, [&w](unsigned int v) { w.u32(v); });
    w.boolean(o.head_locked);
    w.optional(o.adm_source, [&w](const SceneObjectSource& s) {
        gain_source(w, s.gain);
        w.boolean(s.mute_present);
        w.boolean(s.mute);
        w.boolean(s.start_present);
        w.u64(s.start_samples);
        w.u64(s.absolute_start_samples);
        w.optional(s.duration_samples, [&w](std::uint64_t v) { w.u64(v); });
        w.texts(s.child_objects);
        w.boolean(s.has_parent);
    });
    w.f32(o.user_level.gain_multiplier);
    w.optional(o.user_level.mute, [&w](bool v) { w.boolean(v); });
}

inline std::vector<int> encode(const AdmScene& scene) {
    Words w;
    w.u32(1U); // encoding version
    w.u32(scene.info.sample_rate);
    w.u32(scene.info.num_channels);
    w.u64(scene.info.num_frames);
    w.u32(static_cast<std::uint32_t>(scene.info.source_kind));
    w.text(scene.info.input_layout);
    w.texts(scene.info.input_channel_labels);
    w.list(scene.programmes, [&w](const SceneProgramme& p) {
        w.text(p.id);
        w.text(p.name);
        w.texts(p.content_ids);
        w.optional(p.language, [&w](const std::string& s) { w.text(s); });
        w.texts(p.labels);
        w.u64(p.start_sample);
        w.optional(p.end_sample, [&w](std::uint64_t v) { w.u64(v); });
        w.optional(p.loudness, [&w](const SceneLoudnessMetadata& l) { loudness(w, l); });
        w.boolean(p.has_reference_screen);
        w.optional(p.reference_screen, [&w](const ScreenGeometry& s) { screen(w, s); });
    });
    w.list(scene.contents, [&w](const SceneContent& c) {
        w.text(c.id);
        w.text(c.name);
        w.texts(c.object_ids);
        w.optional(c.language, [&w](const std::string& s) { w.text(s); });
        w.texts(c.labels);
        w.optional(c.loudness, [&w](const SceneLoudnessMetadata& l) { loudness(w, l); });
        w.optional(c.dialogue_kind, [&w](const std::string& s) { w.text(s); });
        w.optional(c.content_kind, [&w](const std::string& s) { w.text(s); });
    });
    w.list(scene.objects, [&w](const SceneObject& o) { object(w, o); });
    w.list(scene.hoa_tracks, [&w](const SceneHOATracks& h) {
        w.text(h.object_id);
        w.text(h.pack_format_id);
        w.text(h.normalization);
        w.f64(h.nfc_ref_dist);
        w.boolean(h.screen_ref);
        w.f32(h.gain);
        w.boolean(h.mute);
        w.u64(h.start_sample);
        w.u64(h.end_sample);
        w.list(h.channels, [&w](const SceneHOAChannel& c) {
            w.optional(c.channel_index, [&w](std::uint16_t v) { w.u32(v); });
            w.text(c.track_uid);
            w.u32(static_cast<std::uint32_t>(c.order));
            w.u32(static_cast<std::uint32_t>(c.degree));
            w.list(c.blocks, [&w](const SceneHOAChannelBlock& b) {
                w.f32(b.gain);
                w.u64(b.start_sample);
                w.u64(b.end_sample);
                w.boolean(b.head_locked);
            });
        });
        w.boolean(h.head_locked);
    });
    w.u32(static_cast<std::uint32_t>(scene.import_warnings.size()));
    return w.words();
}

inline void dump(std::string_view stage, const AdmScene& scene) {
    consistency::dump("render-scene/offline." + std::string(stage) + ".i32", encode(scene));
}

} // namespace mradm::scene_checkpoint
#endif
