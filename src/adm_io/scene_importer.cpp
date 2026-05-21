#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <adm/adm.hpp>
#include <adm/parse.hpp>
#include <bw64/bw64.hpp>

#include "adm/io.h"

namespace mradm::io {

namespace {

// AxmlChunk stores data as vector<char> with no string accessor; write() is
// the only public output path.
std::string axml_to_string(const bw64::AxmlChunk& chunk) {
    std::ostringstream buf;
    chunk.write(buf);
    return buf.str();
}

// AudioId::uid() returns a 12-byte fixed-width string padded with spaces (and
// possibly NUL bytes).  libadm may format hex digits A-F as lower-case while
// CHNA/AXML commonly use upper-case, so normalize before matching.
std::string normalize_uid(std::string raw) {
    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\0')) {
        raw.pop_back();
    }
    std::ranges::transform(raw, raw.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return raw;
}

// Build UID string → 0-based channel index from the CHNA chunk.
std::map<std::string, uint16_t> make_uid_map(const std::shared_ptr<bw64::ChnaChunk>& chna) {
    std::map<std::string, uint16_t> result;
    if (!chna) {
        return result;
    }
    for (const auto& entry : chna->audioIds()) {
        std::string uid = normalize_uid(entry.uid());
        if (!uid.empty()) {
            // trackIndex is 1-based in BW64; convert to 0-based
            result[std::move(uid)] = static_cast<uint16_t>(entry.trackIndex() - 1);
        }
    }
    return result;
}

// Convert an adm::Time to a sample offset.  Uses nanosecond representation
// internally; precision is ≪1 sample at all standard audio sample rates.
// NOTE: rtime is relative to AudioObject start; we assume start==0 (the
// common case for single-programme documents).
uint64_t time_to_samples(const adm::Time& t, uint32_t sample_rate) {
    const int64_t ns = t.asNanoseconds().count();
    if (ns <= 0 || sample_rate == 0) {
        return 0;
    }
    constexpr uint64_t k_ns_per_second = 1'000'000'000ULL;
    const auto max = std::numeric_limits<uint64_t>::max();
    const auto ns_u = static_cast<uint64_t>(ns);
    const uint64_t seconds = ns_u / k_ns_per_second;
    const uint64_t remainder = ns_u % k_ns_per_second;
    if (seconds > max / sample_rate) {
        return max;
    }
    const uint64_t whole_samples = seconds * sample_rate;
    const uint64_t fractional_samples = (remainder * sample_rate) / k_ns_per_second;
    if (max - whole_samples < fractional_samples) {
        return max;
    }
    return whole_samples + fractional_samples;
}

uint64_t saturating_add(uint64_t lhs, uint64_t rhs) {
    const auto max = std::numeric_limits<uint64_t>::max();
    if (max - lhs < rhs) {
        return max;
    }
    return lhs + rhs;
}

void append_objects_blocks_from_cf(const std::shared_ptr<adm::AudioChannelFormat>& cf,
                                   SceneTrackRef& ref,
                                   uint32_t sample_rate,
                                   uint64_t obj_start_sample) {
    const auto raw_blocks = cf->getElements<adm::AudioBlockFormatObjects>();
    for (const auto& raw : raw_blocks) {
        SceneObjectBlock block;

        if (raw.has<adm::CartesianPosition>()) {
            const auto& pos = raw.get<adm::CartesianPosition>();
            block.position.cartesian = true;
            block.position.x = static_cast<float>(pos.get<adm::X>().get());
            block.position.y = static_cast<float>(pos.get<adm::Y>().get());
            block.position.z = static_cast<float>(pos.get<adm::Z>().get());
        } else if (raw.has<adm::SphericalPosition>()) {
            const auto& pos = raw.get<adm::SphericalPosition>();
            block.position.cartesian = false;
            block.position.azimuth = static_cast<float>(pos.get<adm::Azimuth>().get());
            block.position.elevation = static_cast<float>(pos.get<adm::Elevation>().get());
            if (pos.has<adm::Distance>()) {
                block.position.distance = static_cast<float>(pos.get<adm::Distance>().get());
            }
        } else {
            continue;
        }

        if (raw.has<adm::Gain>()) {
            block.gain = static_cast<float>(raw.get<adm::Gain>().get());
        }
        if (raw.has<adm::Diffuse>()) {
            block.diffuse = static_cast<float>(raw.get<adm::Diffuse>().get());
        }
        if (raw.has<adm::Width>()) {
            block.width = static_cast<float>(raw.get<adm::Width>().get());
        }
        if (raw.has<adm::Height>()) {
            block.height = static_cast<float>(raw.get<adm::Height>().get());
        }
        if (raw.has<adm::Depth>()) {
            block.depth = static_cast<float>(raw.get<adm::Depth>().get());
        }

        // rtime is DefaultParameter — always present, default 0.
        // Add AudioObject start offset so time is absolute within the file.
        const uint64_t rtime_samples = time_to_samples(raw.get<adm::Rtime>().get(), sample_rate);
        block.start_sample = saturating_add(obj_start_sample, rtime_samples);
        if (raw.has<adm::Duration>()) {
            const uint64_t dur = time_to_samples(raw.get<adm::Duration>().get(), sample_rate);
            block.end_sample = saturating_add(block.start_sample, dur);
        }
        // else: end_sample remains UINT64_MAX (extends to end of file)

        // JumpPosition is DefaultParameter (flag default = false = interpolate).
        const auto jp = raw.get<adm::JumpPosition>();
        block.jump_position = jp.get<adm::JumpPositionFlag>().get();
        if (!jp.isDefault<adm::InterpolationLength>()) {
            block.interp_length_samples =
                time_to_samples(adm::Time{jp.get<adm::InterpolationLength>().get()}, sample_rate);
        }

        // P2 fields: read so renderers can warn and degrade before calling libear.
        block.channel_lock = raw.get<adm::ChannelLock>().get<adm::ChannelLockFlag>().get();
        block.divergence = static_cast<float>(raw.get<adm::ObjectDivergence>().get<adm::Divergence>().get());
        block.screen_ref = raw.get<adm::ScreenRef>().get();

        ref.blocks.push_back(block);
    }
}

void append_direct_speakers_blocks_from_cf(const std::shared_ptr<adm::AudioChannelFormat>& cf,
                                           const std::shared_ptr<adm::AudioPackFormat>& pf,
                                           SceneTrackRef& ref,
                                           uint32_t sample_rate,
                                           uint64_t obj_start_sample) {
    // channelFrequency is a CF-level attribute shared by all blocks within this CF.
    std::optional<float> low_pass_hz;
    if (cf->has<adm::Frequency>() && adm::isLowPass(cf->get<adm::Frequency>())) {
        low_pass_hz = static_cast<float>(cf->get<adm::Frequency>().get<adm::LowPass>().get());
    }

    const auto raw_blocks = cf->getElements<adm::AudioBlockFormatDirectSpeakers>();
    for (const auto& raw : raw_blocks) {
        SceneDirectSpeakersBlock block;
        block.low_pass_hz = low_pass_hz;

        if (pf) {
            block.pack_format_id = adm::formatId(pf->get<adm::AudioPackFormatId>());
        }

        if (raw.has<adm::SpeakerLabels>()) {
            for (const auto& label : raw.get<adm::SpeakerLabels>()) {
                block.speaker_labels.push_back(label.get());
            }
        }

        if (raw.has<adm::SphericalSpeakerPosition>()) {
            const auto& pos = raw.get<adm::SphericalSpeakerPosition>();
            block.has_position = true;
            block.azimuth = static_cast<float>(pos.get<adm::Azimuth>().get());
            block.elevation = static_cast<float>(pos.get<adm::Elevation>().get());
            if (pos.has<adm::Distance>()) {
                block.distance = static_cast<float>(pos.get<adm::Distance>().get());
            }
            if (pos.has<adm::AzimuthMin>()) {
                block.azimuth_min = pos.get<adm::AzimuthMin>().get();
            }
            if (pos.has<adm::AzimuthMax>()) {
                block.azimuth_max = pos.get<adm::AzimuthMax>().get();
            }
            if (pos.has<adm::ElevationMin>()) {
                block.elevation_min = pos.get<adm::ElevationMin>().get();
            }
            if (pos.has<adm::ElevationMax>()) {
                block.elevation_max = pos.get<adm::ElevationMax>().get();
            }
            if (pos.has<adm::DistanceMin>()) {
                block.distance_min = pos.get<adm::DistanceMin>().get();
            }
            if (pos.has<adm::DistanceMax>()) {
                block.distance_max = pos.get<adm::DistanceMax>().get();
            }
        } else if (raw.has<adm::CartesianSpeakerPosition>()) {
            const auto& pos = raw.get<adm::CartesianSpeakerPosition>();
            block.has_position = true;
            const auto cx = static_cast<double>(pos.has<adm::X>() ? pos.get<adm::X>().get() : 0.0F);
            const auto cy = static_cast<double>(pos.has<adm::Y>() ? pos.get<adm::Y>().get() : 0.0F);
            const auto cz = static_cast<double>(pos.has<adm::Z>() ? pos.get<adm::Z>().get() : 0.0F);
            block.azimuth = static_cast<float>(std::atan2(-cx, cy) * (180.0 / std::numbers::pi_v<double>) );
            block.elevation = static_cast<float>(std::atan2(cz, std::sqrt((cx * cx) + (cy * cy))) *
                                                 (180.0 / std::numbers::pi_v<double>) );
            block.distance = static_cast<float>(std::sqrt((cx * cx) + (cy * cy) + (cz * cz)));
        }

        if (raw.has<adm::Gain>()) {
            block.gain = static_cast<float>(raw.get<adm::Gain>().get());
        }

        // rtime is DefaultParameter — always present, default 0.
        const uint64_t rtime = time_to_samples(raw.get<adm::Rtime>().get(), sample_rate);
        block.start_sample = saturating_add(obj_start_sample, rtime);
        if (raw.has<adm::Duration>()) {
            const uint64_t dur = time_to_samples(raw.get<adm::Duration>().get(), sample_rate);
            block.end_sample = saturating_add(block.start_sample, dur);
        }

        ref.ds_blocks.push_back(std::move(block));
    }
}

std::shared_ptr<adm::AudioChannelFormat> channel_format_from_uid(const std::shared_ptr<adm::AudioTrackUid>& uid);

void populate_track_blocks(const std::shared_ptr<adm::AudioTrackUid>& uid,
                           SceneTrackRef& ref,
                           uint32_t sample_rate,
                           uint64_t obj_start_sample,
                           std::set<std::string>& skipped_type_defs) {
    const auto pf = uid->getReference<adm::AudioPackFormat>();
    if (!pf) {
        return;
    }

    const auto append_cf = [&](const std::shared_ptr<adm::AudioChannelFormat>& cf) {
        const auto type = cf->get<adm::TypeDescriptor>();
        if (type == adm::TypeDefinition::OBJECTS) {
            append_objects_blocks_from_cf(cf, ref, sample_rate, obj_start_sample);
        } else if (type == adm::TypeDefinition::DIRECT_SPEAKERS) {
            append_direct_speakers_blocks_from_cf(cf, pf, ref, sample_rate, obj_start_sample);
        } else if (type != adm::TypeDefinition::HOA) {
            // HOA is handled separately by extract_hoa_packs(); anything else is unsupported.
            skipped_type_defs.insert(adm::formatTypeDefinition(type));
        }
    };

    if (const auto cf = channel_format_from_uid(uid); cf != nullptr) {
        append_cf(cf);
        return;
    }

    for (const auto& cf : pf->getReferences<adm::AudioChannelFormat>()) {
        append_cf(cf);
    }
}

uint64_t audio_object_start_samples(const std::shared_ptr<adm::AudioObject>& obj, uint32_t sample_rate) {
    // AudioObject.start is DefaultParameter — always present, default 0.
    return time_to_samples(obj->get<adm::Start>().get(), sample_rate);
}

std::map<std::string, uint64_t> make_object_start_offsets(const std::shared_ptr<adm::Document>& doc,
                                                          uint32_t sample_rate) {
    struct WorkItem {
        std::shared_ptr<adm::AudioObject> object;
        uint64_t inherited_start{0};
    };

    std::map<std::string, uint64_t> offsets;
    std::vector<WorkItem> work;
    for (const auto& content : doc->getElements<adm::AudioContent>()) {
        const auto object_refs = content->getReferences<adm::AudioObject>();
        std::ranges::transform(object_refs, std::back_inserter(work), [](const auto& obj) { return WorkItem{obj, 0}; });
    }

    while (!work.empty()) {
        const auto item = work.back();
        work.pop_back();

        const auto id = adm::formatId(item.object->get<adm::AudioObjectId>());
        const uint64_t start =
            saturating_add(item.inherited_start, audio_object_start_samples(item.object, sample_rate));
        auto [it, inserted] = offsets.emplace(id, start);
        if (!inserted) {
            if (start >= it->second) {
                continue;
            }
            it->second = start;
        }

        const auto child_refs = item.object->getReferences<adm::AudioObject>();
        std::ranges::transform(
            child_refs, std::back_inserter(work), [start](const auto& child) { return WorkItem{child, start}; });
    }

    for (const auto& obj : doc->getElements<adm::AudioObject>()) {
        const auto id = adm::formatId(obj->get<adm::AudioObjectId>());
        offsets.try_emplace(id, audio_object_start_samples(obj, sample_rate));
    }
    return offsets;
}

std::vector<SceneObject> extract_objects(const std::shared_ptr<adm::Document>& doc,
                                         const std::map<std::string, uint16_t>& uid_map,
                                         uint32_t sample_rate,
                                         std::set<std::string>& skipped_type_defs) {
    const auto object_start_offsets = make_object_start_offsets(doc, sample_rate);
    std::vector<SceneObject> result;
    for (const auto& obj : doc->getElements<adm::AudioObject>()) {
        SceneObject out;
        out.id = adm::formatId(obj->get<adm::AudioObjectId>());
        if (obj->has<adm::AudioObjectName>()) {
            out.name = obj->get<adm::AudioObjectName>().get();
        }
        if (obj->has<adm::Gain>()) {
            out.gain = static_cast<float>(obj->get<adm::Gain>().get());
        }
        if (obj->has<adm::Mute>()) {
            out.mute = obj->get<adm::Mute>().get();
        }
        if (obj->has<adm::PositionOffset>()) {
            const auto& po = obj->get<adm::PositionOffset>();
            ScenePositionOffset offset;
            if (adm::isSpherical(po)) {
                const auto& spo = boost::get<adm::SphericalPositionOffset>(po);
                if (spo.has<adm::AzimuthOffset>()) {
                    offset.azimuth = spo.get<adm::AzimuthOffset>().get();
                }
                if (spo.has<adm::ElevationOffset>()) {
                    offset.elevation = spo.get<adm::ElevationOffset>().get();
                }
                if (spo.has<adm::DistanceOffset>()) {
                    offset.distance = spo.get<adm::DistanceOffset>().get();
                }
            } else {
                offset.cartesian = true;
                const auto& cpo = boost::get<adm::CartesianPositionOffset>(po);
                if (cpo.has<adm::XOffset>()) {
                    offset.x = cpo.get<adm::XOffset>().get();
                }
                if (cpo.has<adm::YOffset>()) {
                    offset.y = cpo.get<adm::YOffset>().get();
                }
                if (cpo.has<adm::ZOffset>()) {
                    offset.z = cpo.get<adm::ZOffset>().get();
                }
            }
            out.position_offset = offset;
        }
        const auto start_it = object_start_offsets.find(out.id);
        const uint64_t obj_start = (start_it != object_start_offsets.end()) ? start_it->second : 0;
        if (obj->has<adm::Duration>()) {
            const uint64_t dur = time_to_samples(obj->get<adm::Duration>().get(), sample_rate);
            out.end_sample = saturating_add(obj_start, dur);
        }
        if (obj->has<adm::Labels>()) {
            for (const auto& label : obj->get<adm::Labels>()) {
                if (label.has<adm::LabelValue>()) {
                    out.labels.push_back(label.get<adm::LabelValue>().get());
                }
            }
        }
        if (obj->has<adm::Importance>()) {
            out.importance = obj->get<adm::Importance>().get();
        }
        if (obj->has<adm::DialogueId>()) {
            out.dialogue_id = obj->get<adm::DialogueId>().get();
        }
        for (const auto& uid : obj->getReferences<adm::AudioTrackUid>()) {
            SceneTrackRef ref;
            ref.track_uid = adm::formatId(uid->get<adm::AudioTrackUidId>());
            auto it = uid_map.find(normalize_uid(ref.track_uid));
            if (it != uid_map.end()) {
                ref.channel_index = it->second;
            }
            populate_track_blocks(uid, ref, sample_rate, obj_start, skipped_type_defs);
            out.tracks.push_back(std::move(ref));
        }
        result.push_back(std::move(out));
    }
    return result;
}

// Convert DialogueId + optional sub-kind to human-readable strings.
std::pair<std::optional<std::string>, std::optional<std::string>>
dialogue_strings(const std::shared_ptr<adm::AudioContent>& c) {
    if (!c->has<adm::DialogueId>()) {
        return {};
    }
    const unsigned int id = c->get<adm::DialogueId>().get();
    std::optional<std::string> dk;
    std::optional<std::string> ck;
    if (id == 0) {
        dk = "non-dialogue";
        if (c->has<adm::NonDialogueContentKind>()) {
            switch (c->get<adm::NonDialogueContentKind>().get()) {
            case 1:
                ck = "music";
                break;
            case 2:
                ck = "effect";
                break;
            default:
                break;
            }
        }
    } else if (id == 1) {
        dk = "dialogue";
        if (c->has<adm::DialogueContentKind>()) {
            switch (c->get<adm::DialogueContentKind>().get()) {
            case 1:
                ck = "dialogue";
                break;
            case 2:
                ck = "voiceover";
                break;
            case 3:
                ck = "spoken-subtitle";
                break;
            case 4:
                ck = "audio-description";
                break;
            case 5:
                ck = "commentary";
                break;
            case 6:
                ck = "emergency";
                break;
            default:
                break;
            }
        }
    } else if (id == 2) {
        dk = "mixed";
        if (c->has<adm::MixedContentKind>()) {
            switch (c->get<adm::MixedContentKind>().get()) {
            case 1:
                ck = "complete-main";
                break;
            case 2:
                ck = "mixed";
                break;
            case 3:
                ck = "hearing-impaired";
                break;
            default:
                break;
            }
        }
    }
    return {dk, ck};
}

std::vector<SceneContent> extract_contents(const std::shared_ptr<adm::Document>& doc) {
    std::vector<SceneContent> result;
    for (const auto& c : doc->getElements<adm::AudioContent>()) {
        SceneContent out;
        out.id = adm::formatId(c->get<adm::AudioContentId>());
        if (c->has<adm::AudioContentName>()) {
            out.name = c->get<adm::AudioContentName>().get();
        }
        for (const auto& obj : c->getReferences<adm::AudioObject>()) {
            out.object_ids.push_back(adm::formatId(obj->get<adm::AudioObjectId>()));
        }
        if (c->has<adm::AudioContentLanguage>()) {
            out.language = c->get<adm::AudioContentLanguage>().get();
        }
        if (c->has<adm::Labels>()) {
            for (const auto& label : c->get<adm::Labels>()) {
                if (label.has<adm::LabelValue>()) {
                    out.labels.push_back(label.get<adm::LabelValue>().get());
                }
            }
        }
        if (c->has<adm::LoudnessMetadatas>()) {
            const auto& vec = c->get<adm::LoudnessMetadatas>();
            if (!vec.empty()) {
                const auto& src = vec[0];
                SceneLoudnessMetadata lm;
                if (src.has<adm::IntegratedLoudness>()) {
                    lm.integrated_loudness = static_cast<float>(src.get<adm::IntegratedLoudness>().get());
                }
                if (src.has<adm::MaxTruePeak>()) {
                    lm.max_true_peak = static_cast<float>(src.get<adm::MaxTruePeak>().get());
                }
                if (src.has<adm::LoudnessRange>()) {
                    lm.loudness_range = static_cast<float>(src.get<adm::LoudnessRange>().get());
                }
                if (src.has<adm::MaxMomentary>()) {
                    lm.max_momentary = static_cast<float>(src.get<adm::MaxMomentary>().get());
                }
                if (src.has<adm::MaxShortTerm>()) {
                    lm.max_short_term = static_cast<float>(src.get<adm::MaxShortTerm>().get());
                }
                if (src.has<adm::DialogueLoudness>()) {
                    lm.dialogue_loudness = static_cast<float>(src.get<adm::DialogueLoudness>().get());
                }
                if (src.has<adm::LoudnessMethod>()) {
                    lm.loudness_method = src.get<adm::LoudnessMethod>().get();
                }
                out.loudness = std::move(lm);
            }
        }
        auto [dk, ck] = dialogue_strings(c);
        out.dialogue_kind = std::move(dk);
        out.content_kind = std::move(ck);
        result.push_back(std::move(out));
    }
    return result;
}

std::vector<SceneProgramme> extract_programmes(const std::shared_ptr<adm::Document>& doc, uint32_t sample_rate) {
    std::vector<SceneProgramme> result;
    for (const auto& p : doc->getElements<adm::AudioProgramme>()) {
        SceneProgramme out;
        out.id = adm::formatId(p->get<adm::AudioProgrammeId>());
        if (p->has<adm::AudioProgrammeName>()) {
            out.name = p->get<adm::AudioProgrammeName>().get();
        }
        for (const auto& c : p->getReferences<adm::AudioContent>()) {
            out.content_ids.push_back(adm::formatId(c->get<adm::AudioContentId>()));
        }
        if (p->has<adm::AudioProgrammeLanguage>()) {
            out.language = p->get<adm::AudioProgrammeLanguage>().get();
        }
        if (p->has<adm::Labels>()) {
            for (const auto& label : p->get<adm::Labels>()) {
                if (label.has<adm::LabelValue>()) {
                    out.labels.push_back(label.get<adm::LabelValue>().get());
                }
            }
        }
        if (!p->isDefault<adm::Start>()) {
            out.start_sample = time_to_samples(p->get<adm::Start>().get(), sample_rate);
        }
        if (p->has<adm::End>()) {
            out.end_sample = time_to_samples(p->get<adm::End>().get(), sample_rate);
        }
        if (p->has<adm::LoudnessMetadatas>()) {
            const auto& vec = p->get<adm::LoudnessMetadatas>();
            if (!vec.empty()) {
                const auto& src = vec[0];
                SceneLoudnessMetadata lm;
                if (src.has<adm::IntegratedLoudness>()) {
                    lm.integrated_loudness = static_cast<float>(src.get<adm::IntegratedLoudness>().get());
                }
                if (src.has<adm::MaxTruePeak>()) {
                    lm.max_true_peak = static_cast<float>(src.get<adm::MaxTruePeak>().get());
                }
                if (src.has<adm::LoudnessRange>()) {
                    lm.loudness_range = static_cast<float>(src.get<adm::LoudnessRange>().get());
                }
                if (src.has<adm::MaxMomentary>()) {
                    lm.max_momentary = static_cast<float>(src.get<adm::MaxMomentary>().get());
                }
                if (src.has<adm::MaxShortTerm>()) {
                    lm.max_short_term = static_cast<float>(src.get<adm::MaxShortTerm>().get());
                }
                if (src.has<adm::DialogueLoudness>()) {
                    lm.dialogue_loudness = static_cast<float>(src.get<adm::DialogueLoudness>().get());
                }
                if (src.has<adm::LoudnessMethod>()) {
                    lm.loudness_method = src.get<adm::LoudnessMethod>().get();
                }
                out.loudness = std::move(lm);
            }
        }
        out.has_reference_screen = p->has<adm::AudioProgrammeReferenceScreen>();
        result.push_back(std::move(out));
    }
    return result;
}

// Traverse UID → TrackFormat → StreamFormat → ChannelFormat; returns nullptr when
// any link in the chain is absent.
std::shared_ptr<adm::AudioChannelFormat> channel_format_from_uid(const std::shared_ptr<adm::AudioTrackUid>& uid) {
    const auto tf = uid->getReference<adm::AudioTrackFormat>();
    if (!tf) {
        return nullptr;
    }
    const auto sf = tf->getReference<adm::AudioStreamFormat>();
    if (!sf) {
        return nullptr;
    }
    return sf->getReference<adm::AudioChannelFormat>();
}

// Read order/degree from the first AudioBlockFormatHoa (fixed per channel), then
// append one SceneHOAChannelBlock per block (rtime/duration/gain).  Returns true
// when at least one block was found; false means the caller should skip this channel.
bool populate_hoa_channel_from_cf(const std::shared_ptr<adm::AudioChannelFormat>& cf,
                                  SceneHOAChannel& ch,
                                  uint64_t pack_start,
                                  uint64_t pack_end,
                                  uint32_t sample_rate) {
    const auto hoa_blocks = cf->getElements<adm::AudioBlockFormatHoa>();
    if (hoa_blocks.empty()) {
        return false;
    }
    // order/degree are fixed per channel — read once from the first block.
    const auto& first = hoa_blocks.front();
    if (first.has<adm::Order>()) {
        ch.order = first.get<adm::Order>().get();
    }
    if (first.has<adm::Degree>()) {
        ch.degree = first.get<adm::Degree>().get();
    }
    ch.blocks.reserve(hoa_blocks.size());
    for (const auto& blk : hoa_blocks) {
        SceneHOAChannelBlock b;
        b.gain = static_cast<float>(blk.get<adm::Gain>().get());
        const uint64_t rtime = time_to_samples(blk.get<adm::Rtime>().get(), sample_rate);
        b.start_sample = saturating_add(pack_start, rtime);
        if (blk.has<adm::Duration>()) {
            const uint64_t dur = time_to_samples(blk.get<adm::Duration>().get(), sample_rate);
            b.end_sample = std::min(saturating_add(b.start_sample, dur), pack_end);
        } else {
            b.end_sample = pack_end;
        }
        ch.blocks.push_back(b);
    }
    return true;
}

// Override pack normalization/nfcRefDist/screenRef with block values if non-default
// (BS.2127 §5.2.7.3: block-level takes precedence over pack-level).
void apply_hoa_block_pack_metadata(const std::shared_ptr<adm::AudioChannelFormat>& cf, SceneHOATracks& pack) {
    const auto hoa_blocks = cf->getElements<adm::AudioBlockFormatHoa>();
    if (hoa_blocks.empty()) {
        return;
    }
    const auto& blk = hoa_blocks.front();
    if (!blk.isDefault<adm::Normalization>()) {
        pack.normalization = blk.get<adm::Normalization>().get();
    }
    if (!blk.isDefault<adm::NfcRefDist>()) {
        pack.nfc_ref_dist = static_cast<double>(blk.get<adm::NfcRefDist>().get());
    }
    if (!blk.isDefault<adm::ScreenRef>()) {
        pack.screen_ref = blk.get<adm::ScreenRef>().get();
    }
}

std::vector<SceneHOATracks> extract_hoa_packs(const std::shared_ptr<adm::Document>& doc,
                                              const std::map<std::string, uint16_t>& uid_map,
                                              uint32_t sample_rate) {
    const auto object_start_offsets = make_object_start_offsets(doc, sample_rate);
    std::vector<SceneHOATracks> result;

    for (const auto& obj : doc->getElements<adm::AudioObject>()) {
        // Group UIDs by HOA pack format ID; also retain the pack format pointer.
        std::map<std::string, std::shared_ptr<adm::AudioPackFormat>> pack_formats;
        std::map<std::string, std::vector<std::shared_ptr<adm::AudioTrackUid>>> pack_to_uids;
        for (const auto& uid : obj->getReferences<adm::AudioTrackUid>()) {
            const auto pf = uid->getReference<adm::AudioPackFormat>();
            if (!pf || pf->get<adm::TypeDescriptor>() != adm::TypeDefinition::HOA) {
                continue;
            }
            const auto pf_id = adm::formatId(pf->get<adm::AudioPackFormatId>());
            pack_to_uids[pf_id].push_back(uid);
            pack_formats.emplace(pf_id, pf);
        }
        if (pack_to_uids.empty()) {
            continue;
        }

        const auto obj_id = adm::formatId(obj->get<adm::AudioObjectId>());
        const auto start_it = object_start_offsets.find(obj_id);
        const uint64_t obj_start = (start_it != object_start_offsets.end()) ? start_it->second : 0;

        for (auto& [pf_id, uids] : pack_to_uids) {
            SceneHOATracks pack;
            pack.object_id = obj_id;
            pack.pack_format_id = pf_id;

            if (obj->has<adm::Gain>()) {
                pack.gain = static_cast<float>(obj->get<adm::Gain>().get());
            }
            if (obj->has<adm::Mute>()) {
                pack.mute = obj->get<adm::Mute>().get();
            }
            pack.start_sample = obj_start;
            if (obj->has<adm::Duration>()) {
                const uint64_t dur = time_to_samples(obj->get<adm::Duration>().get(), sample_rate);
                pack.end_sample = saturating_add(obj_start, dur);
            }

            // Seed normalization/nfcRefDist/screenRef from AudioPackFormatHoa
            // (BS.2127 §5.2.7.3: pack-level is the fallback; block-level overrides).
            const auto* hoa_pf = dynamic_cast<const adm::AudioPackFormatHoa*>(pack_formats.at(pf_id).get());
            if (hoa_pf != nullptr) {
                pack.normalization = hoa_pf->get<adm::Normalization>().get();
                pack.nfc_ref_dist = static_cast<double>(hoa_pf->get<adm::NfcRefDist>().get());
                pack.screen_ref = hoa_pf->get<adm::ScreenRef>().get();
            }

            bool got_block_metadata = false;
            for (const auto& uid : uids) {
                SceneHOAChannel ch;
                ch.track_uid = adm::formatId(uid->get<adm::AudioTrackUidId>());
                const auto uid_it = uid_map.find(normalize_uid(ch.track_uid));
                if (uid_it != uid_map.end()) {
                    ch.channel_index = uid_it->second;
                }

                const auto cf = channel_format_from_uid(uid);
                if (cf != nullptr) {
                    const bool found =
                        populate_hoa_channel_from_cf(cf, ch, pack.start_sample, pack.end_sample, sample_rate);
                    if (found && !got_block_metadata) {
                        apply_hoa_block_pack_metadata(cf, pack);
                        got_block_metadata = true;
                    }
                }
                // Only include channels with at least one decoded block.
                // UIDs with no HOA block data (broken chain, wrong type) are
                // silently skipped to avoid polluting the pack with phantom
                // (order=0, degree=0) channels that would corrupt the decode matrix.
                if (!ch.blocks.empty()) {
                    pack.channels.push_back(std::move(ch));
                }
            }

            if (!pack.channels.empty()) {
                result.push_back(std::move(pack));
            }
        }
    }
    return result;
}

} // namespace

Result<AdmScene> import_scene(const std::string& path) {
    try {
        auto reader = bw64::readFile(path);

        SceneInfo info;
        info.file_path = path;
        info.sample_rate = reader->sampleRate();
        info.num_channels = reader->channels();
        info.num_frames = reader->numberOfFrames();

        const auto axml = reader->axmlChunk();
        if (!axml || axml->size() == 0) {
            return make_error(ErrorCode::io_error, "axml chunk 缺失或为空", "input=" + path);
        }

        std::string xml_str = axml_to_string(*axml);
        std::istringstream xml_stream{xml_str};
        auto document = adm::parseXml(xml_stream);

        auto uid_map = make_uid_map(reader->chnaChunk());

        AdmScene scene;
        scene.info = std::move(info);
        scene.programmes = extract_programmes(document, reader->sampleRate());
        scene.contents = extract_contents(document);
        std::set<std::string> skipped_type_defs;
        scene.objects = extract_objects(document, uid_map, reader->sampleRate(), skipped_type_defs);
        scene.hoa_tracks = extract_hoa_packs(document, uid_map, reader->sampleRate());
        for (const auto& type_name : skipped_type_defs) {
            scene.import_warnings.push_back("typeDefinition=" + type_name +
                                            " is not supported — tracks of this type are silently skipped");
        }

        return scene;

    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error, std::string("解析 BW64/ADM 文件失败：") + e.what(), "input=" + path);
    } catch (...) {
        return make_error(ErrorCode::io_error, "解析 BW64/ADM 文件时发生未知异常", "input=" + path);
    }
}

Result<std::string> get_axml(const std::string& path) {
    try {
        auto reader = bw64::readFile(path);
        const auto axml = reader->axmlChunk();
        if (!axml || axml->size() == 0) {
            return make_error(ErrorCode::io_error, "axml chunk 缺失或为空", "input=" + path);
        }
        return axml_to_string(*axml);
    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error, std::string("读取 AXML 失败：") + e.what(), "input=" + path);
    }
}

} // namespace mradm::io
