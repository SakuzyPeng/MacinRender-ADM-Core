#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
#include <adm/write.hpp>
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

// Convert an adm::Time to the nearest sample offset.  ADM authored by DAWs often
// stores sample-aligned times as decimal seconds, so flooring each timestamp can
// create one-sample holes at dense block boundaries.
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
    const uint64_t fractional_samples = ((remainder * sample_rate) + (k_ns_per_second / 2U)) / k_ns_per_second;
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

        // Objects panning modifiers. libear exposes these but throws for non-default
        // values, so renderers consume them in project-owned preprocessing.
        const auto channel_lock = raw.get<adm::ChannelLock>();
        block.channel_lock = channel_lock.get<adm::ChannelLockFlag>().get();
        if (channel_lock.has<adm::MaxDistance>()) {
            block.channel_lock_max_distance = static_cast<float>(channel_lock.get<adm::MaxDistance>().get());
        }

        const auto divergence = raw.get<adm::ObjectDivergence>();
        block.divergence = static_cast<float>(divergence.get<adm::Divergence>().get());
        block.divergence_azimuth_range = static_cast<float>(divergence.get<adm::AzimuthRange>().get());
        block.divergence_position_range = static_cast<float>(divergence.get<adm::PositionRange>().get());
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

namespace {

// ---- semantic write-back (export) ----
// 下列 helper 镜像 import 侧的遍历（extract_objects → populate_track_blocks →
// append_*_from_cf），但方向相反：对每个 ADM 元素，只把 effective 相对 original
// 改变的字段写回，未变字段保持文档原值，避免把 importer 的有损转换固化进输出。
// original / effective 同源（同一次 import_scene），故遍历顺序与文档逐一对齐。

void patch_objects_block(adm::AudioBlockFormatObjects& raw, const SceneObjectBlock& orig, const SceneObjectBlock& eff) {
    if (eff.gain != orig.gain) {
        raw.set(adm::Gain(eff.gain));
    }
    if (eff.diffuse != orig.diffuse) {
        raw.set(adm::Diffuse(eff.diffuse));
    }
    if (eff.width != orig.width) {
        raw.set(adm::Width(eff.width));
    }
    if (eff.height != orig.height) {
        raw.set(adm::Height(eff.height));
    }
    if (eff.depth != orig.depth) {
        raw.set(adm::Depth(eff.depth));
    }
    if (eff.divergence != orig.divergence || eff.divergence_azimuth_range != orig.divergence_azimuth_range ||
        eff.divergence_position_range != orig.divergence_position_range) {
        // get-modify-set 保留未改子字段。
        auto divergence = raw.get<adm::ObjectDivergence>();
        divergence.set(adm::Divergence(eff.divergence));
        divergence.set(adm::AzimuthRange(eff.divergence_azimuth_range));
        divergence.set(adm::PositionRange(eff.divergence_position_range));
        raw.set(divergence);
    }
    if (eff.channel_lock != orig.channel_lock || eff.channel_lock_max_distance != orig.channel_lock_max_distance) {
        auto channel_lock = raw.get<adm::ChannelLock>();
        channel_lock.set(adm::ChannelLockFlag(eff.channel_lock));
        if (eff.channel_lock_max_distance.has_value()) {
            channel_lock.set(adm::MaxDistance(*eff.channel_lock_max_distance));
        }
        raw.set(channel_lock);
    }
    if (eff.jump_position != orig.jump_position) {
        // interpolationLength 阶段 1 不写回，get-modify-set 保留原插值长度。
        auto jump = raw.get<adm::JumpPosition>();
        jump.set(adm::JumpPositionFlag(eff.jump_position));
        raw.set(jump);
    }
    // Position write-back is intentionally deferred: applying semantic position
    // policy can change coordinate representation, which needs a separate design.
}

void patch_direct_speakers_block(adm::AudioBlockFormatDirectSpeakers& raw,
                                 const SceneDirectSpeakersBlock& orig,
                                 const SceneDirectSpeakersBlock& eff) {
    if (eff.gain != orig.gain) {
        raw.set(adm::Gain(eff.gain));
    }
    // Position write-back is intentionally deferred for DirectSpeakers as well.
}

void patch_track(const std::shared_ptr<adm::AudioTrackUid>& uid, const SceneTrackRef& orig, const SceneTrackRef& eff) {
    const auto pf = uid->getReference<adm::AudioPackFormat>();
    if (!pf) {
        return;
    }
    std::size_t obj_block_index = 0;
    std::size_t ds_block_index = 0;
    const auto patch_cf = [&](const std::shared_ptr<adm::AudioChannelFormat>& cf) {
        const auto type = cf->get<adm::TypeDescriptor>();
        if (type == adm::TypeDefinition::OBJECTS) {
            for (auto& raw : cf->getElements<adm::AudioBlockFormatObjects>()) {
                if (obj_block_index >= orig.blocks.size() || obj_block_index >= eff.blocks.size()) {
                    break;
                }
                patch_objects_block(raw, orig.blocks[obj_block_index], eff.blocks[obj_block_index]);
                ++obj_block_index;
            }
        } else if (type == adm::TypeDefinition::DIRECT_SPEAKERS) {
            for (auto& raw : cf->getElements<adm::AudioBlockFormatDirectSpeakers>()) {
                if (ds_block_index >= orig.ds_blocks.size() || ds_block_index >= eff.ds_blocks.size()) {
                    break;
                }
                patch_direct_speakers_block(raw, orig.ds_blocks[ds_block_index], eff.ds_blocks[ds_block_index]);
                ++ds_block_index;
            }
        }
        // HOA / 其它 typeDefinition：阶段 1 不写回，文档原样保留。
    };

    if (const auto cf = channel_format_from_uid(uid); cf != nullptr) {
        patch_cf(cf);
        return;
    }
    for (const auto& cf : pf->getReferences<adm::AudioChannelFormat>()) {
        patch_cf(cf);
    }
}

void patch_audio_object(const std::shared_ptr<adm::AudioObject>& obj, const SceneObject& orig, const SceneObject& eff) {
    if (eff.gain != orig.gain) {
        obj->set(adm::Gain(eff.gain));
    }
    if (eff.mute != orig.mute) {
        obj->set(adm::Mute(eff.mute));
    }
}

// ---- chunk-level BWF rewrite (bit-exact PCM) ----
// 仅替换 axml chunk 的 payload，其余 chunk（data / chna / fmt / bext / JUNK / 未知）
// 的原始字节逐块复制，绝不解码 / 重编码 PCM —— 这是音频真正 bit-exact 的唯一方式
// （libbw64 的 float read/write 24-bit 量化不对称，往返会漂移 ~1 LSB）。

uint32_t read_u32_le(const char* p) {
    return static_cast<uint32_t>(static_cast<unsigned char>(p[0])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8U) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16U) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24U);
}

uint64_t read_u64_le(const char* p) {
    return static_cast<uint64_t>(read_u32_le(p)) | (static_cast<uint64_t>(read_u32_le(p + 4)) << 32U);
}

void write_u32_le(std::ostream& os, uint32_t value) {
    const std::array<char, 4> bytes{static_cast<char>(value & 0xFFU),
                                    static_cast<char>((value >> 8U) & 0xFFU),
                                    static_cast<char>((value >> 16U) & 0xFFU),
                                    static_cast<char>((value >> 24U) & 0xFFU)};
    os.write(bytes.data(), bytes.size());
}

bool fourcc_eq(const std::array<char, 4>& id, const char* tag) {
    return id[0] == tag[0] && id[1] == tag[1] && id[2] == tag[2] && id[3] == tag[3];
}

struct SrcChunk {
    std::array<char, 4> id{};
    uint64_t payload_offset{0};
    uint64_t payload_size{0};
};

// Rewrite src_path → dst_path, replacing only the axml chunk payload with new_axml.
// All other chunks (notably data and chna) are copied byte-for-byte. Top-level
// RIFF/BW64 size and ds64.bw64Size are recomputed; chunk padding is preserved.
// NOLINTBEGIN(readability-function-size)
Result<void>
rewrite_bwf_replacing_axml(const std::string& src_path, const std::string& dst_path, const std::string& new_axml) {
    std::ifstream in(src_path, std::ios::binary);
    if (!in) {
        return make_error(ErrorCode::io_error, "无法打开源文件", "input=" + src_path);
    }

    std::array<char, 12> header{};
    in.read(header.data(), header.size());
    if (in.gcount() != static_cast<std::streamsize>(header.size()) || header[8] != 'W' || header[9] != 'A' ||
        header[10] != 'V' || header[11] != 'E') {
        return make_error(ErrorCode::io_error, "不是有效的 RIFF/WAVE 文件", "input=" + src_path);
    }
    const std::array<char, 4> riff_id{header[0], header[1], header[2], header[3]};
    const bool src_is_rf64 = fourcc_eq(riff_id, "RF64") || fourcc_eq(riff_id, "BW64");

    in.seekg(0, std::ios::end);
    const auto file_size = static_cast<uint64_t>(in.tellg());

    // Parse the chunk table (offsets/sizes only; payloads stay on disk).
    std::vector<SrcChunk> chunks;
    bool have_ds64 = false;
    uint64_t ds64_data_size = 0;
    uint32_t ds64_table_length = 0;
    uint64_t pos = 12;
    while (pos + 8 <= file_size) {
        std::array<char, 8> ch{};
        in.seekg(static_cast<std::streamoff>(pos));
        in.read(ch.data(), ch.size());
        if (in.gcount() != static_cast<std::streamsize>(ch.size())) {
            break;
        }
        SrcChunk chunk;
        chunk.id = {ch[0], ch[1], ch[2], ch[3]};
        const uint32_t size32 = read_u32_le(ch.data() + 4);
        chunk.payload_offset = pos + 8;
        chunk.payload_size = size32;

        if (fourcc_eq(chunk.id, "ds64")) {
            // [bw64Size:8][dataSize:8][sampleCount:8][tableLength:4]
            std::array<char, 28> ds{};
            in.read(ds.data(), ds.size());
            if (in.gcount() == static_cast<std::streamsize>(ds.size())) {
                have_ds64 = true;
                ds64_data_size = read_u64_le(ds.data() + 8);
                ds64_table_length = read_u32_le(ds.data() + 24);
            }
        } else if (fourcc_eq(chunk.id, "data") && src_is_rf64 && size32 == 0xFFFFFFFFU) {
            chunk.payload_size = have_ds64 ? ds64_data_size : size32;
        }
        chunks.push_back(chunk);
        pos = chunk.payload_offset + chunk.payload_size + (chunk.payload_size & 1ULL);
    }

    // Output layout: every chunk keeps its size except axml.
    uint64_t out_body = 4; // "WAVE"
    bool have_axml = false;
    for (const auto& chunk : chunks) {
        const bool is_axml = fourcc_eq(chunk.id, "axml");
        have_axml = have_axml || is_axml;
        const uint64_t out_size = is_axml ? new_axml.size() : chunk.payload_size;
        out_body += 8 + out_size + (out_size & 1ULL);
    }
    if (!have_axml) {
        return make_error(ErrorCode::io_error, "源文件缺少 axml chunk", "input=" + src_path);
    }
    const uint64_t out_riff_size = out_body;
    const bool out_is_bw64 = src_is_rf64;
    if (!out_is_bw64 && out_riff_size > 0xFFFFFFFFULL) {
        return make_error(
            ErrorCode::unsupported, "输出超过 4GB 但源非 BW64/RF64，暂不支持自动升级", "output=" + dst_path);
    }
    if (out_is_bw64 && !have_ds64) {
        return make_error(ErrorCode::io_error, "BW64/RF64 文件缺少 ds64 chunk", "input=" + src_path);
    }
    // A non-empty ds64 size table records 64-bit sizes for non-data chunks > 4GB
    // (e.g. a > 4GB axml). We only rewrite ds64.bw64Size, not its table, so we
    // cannot keep such an entry in sync — refuse rather than emit a corrupt table.
    // (tableLength is 0 for all realistic ADM BWF files; only data exceeds 4GB.)
    if (out_is_bw64 && ds64_table_length > 0) {
        return make_error(ErrorCode::unsupported,
                          "ds64 size table 非空（含超过 4GB 的非 data chunk），写回暂不支持",
                          "input=" + src_path);
    }

    const std::string tmp_path = dst_path + ".export.tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return make_error(ErrorCode::io_error, "无法创建临时输出文件", "output=" + tmp_path);
        }

        // Preserve the source container FourCC (RF64 stays RF64, BW64 stays BW64);
        // out_is_bw64 implies src_is_rf64, so riff_id is a valid 64-bit tag here.
        out.write(out_is_bw64 ? riff_id.data() : "RIFF", 4);
        write_u32_le(out, out_is_bw64 ? 0xFFFFFFFFU : static_cast<uint32_t>(out_riff_size));
        out.write("WAVE", 4);

        std::vector<char> buffer(static_cast<std::size_t>(1U) << 16U);
        for (const auto& chunk : chunks) {
            const bool is_axml = fourcc_eq(chunk.id, "axml");
            const bool is_ds64 = fourcc_eq(chunk.id, "ds64");
            const bool is_data = fourcc_eq(chunk.id, "data");
            const uint64_t out_size = is_axml ? new_axml.size() : chunk.payload_size;

            uint32_t size_field = 0;
            if (out_is_bw64 && is_data && out_size > 0xFFFFFFFFULL) {
                size_field = 0xFFFFFFFFU; // sentinel; real size lives in ds64.dataSize
            } else if (out_size > 0xFFFFFFFFULL) {
                return make_error(ErrorCode::unsupported, "非 data chunk 超过 4GB，暂不支持", "input=" + src_path);
            } else {
                size_field = static_cast<uint32_t>(out_size);
            }

            out.write(chunk.id.data(), 4);
            write_u32_le(out, size_field);

            if (is_axml) {
                out.write(new_axml.data(), static_cast<std::streamsize>(new_axml.size()));
            } else if (is_ds64 && out_is_bw64) {
                // Rewrite ds64: update bw64Size (first 8 bytes) to the new RIFF size;
                // dataSize / sampleCount / table stay as-is (PCM bytes are unchanged).
                std::vector<char> ds(static_cast<std::size_t>(chunk.payload_size));
                in.seekg(static_cast<std::streamoff>(chunk.payload_offset));
                in.read(ds.data(), static_cast<std::streamsize>(chunk.payload_size));
                if (in.gcount() != static_cast<std::streamsize>(chunk.payload_size)) {
                    return make_error(ErrorCode::io_error, "读取 ds64 chunk 失败", "input=" + src_path);
                }
                uint64_t value = out_riff_size;
                for (std::size_t i = 0; i < 8 && i < ds.size(); ++i) {
                    ds[i] = static_cast<char>(value & 0xFFULL);
                    value >>= 8U;
                }
                out.write(ds.data(), static_cast<std::streamsize>(ds.size()));
            } else {
                // Byte-for-byte stream copy (data / chna / fmt / bext / JUNK / unknown).
                uint64_t remaining = chunk.payload_size;
                in.seekg(static_cast<std::streamoff>(chunk.payload_offset));
                while (remaining > 0) {
                    const uint64_t want = std::min<uint64_t>(remaining, buffer.size());
                    in.read(buffer.data(), static_cast<std::streamsize>(want));
                    if (in.gcount() != static_cast<std::streamsize>(want)) {
                        return make_error(ErrorCode::io_error, "读取 chunk 数据失败", "input=" + src_path);
                    }
                    out.write(buffer.data(), static_cast<std::streamsize>(want));
                    remaining -= want;
                }
            }

            if ((out_size & 1ULL) != 0U) {
                out.put('\0'); // RIFF chunks pad payloads with an odd length to even.
            }
        }

        out.flush();
        if (!out) {
            std::error_code rm;
            std::filesystem::remove(tmp_path, rm);
            return make_error(ErrorCode::io_error, "写出 ADM 文件失败", "output=" + tmp_path);
        }
    } // out closed/flushed here
    in.close();

    // Atomic-ish replace: write temp first, then rename onto the target so a failure
    // mid-write never corrupts the destination (or the source when dst == src).
    std::error_code ec;
    std::filesystem::rename(tmp_path, dst_path, ec);
    if (ec) {
        std::filesystem::copy_file(tmp_path, dst_path, std::filesystem::copy_options::overwrite_existing, ec);
        std::error_code rm;
        std::filesystem::remove(tmp_path, rm);
        if (ec) {
            return make_error(ErrorCode::io_error, "替换输出文件失败：" + ec.message(), "output=" + dst_path);
        }
    }
    return {};
}
// NOLINTEND(readability-function-size)

} // namespace

Result<void> write_scene(const std::string& src_path,
                         const AdmScene& original,
                         const AdmScene& effective,
                         const std::string& dst_path) {
    try {
        std::string xml_str;
        {
            auto reader = bw64::readFile(src_path);
            const auto axml = reader->axmlChunk();
            if (!axml || axml->size() == 0) {
                return make_error(ErrorCode::io_error, "axml chunk 缺失或为空", "input=" + src_path);
            }
            xml_str = axml_to_string(*axml);
        } // reader 关闭：rewrite 阶段以独立的二进制流重新打开源文件

        std::istringstream xml_stream{xml_str};
        auto document = adm::parseXml(xml_stream);

        // 镜像 import 遍历，逐对象 / 轨道 / 块写回 effective 相对 original 的变化。
        std::size_t object_index = 0;
        for (const auto& obj : document->getElements<adm::AudioObject>()) {
            if (object_index >= original.objects.size() || object_index >= effective.objects.size()) {
                break;
            }
            const auto& orig_obj = original.objects[object_index];
            const auto& eff_obj = effective.objects[object_index];
            patch_audio_object(obj, orig_obj, eff_obj);

            std::size_t track_index = 0;
            for (const auto& uid : obj->getReferences<adm::AudioTrackUid>()) {
                if (track_index >= orig_obj.tracks.size() || track_index >= eff_obj.tracks.size()) {
                    break;
                }
                patch_track(uid, orig_obj.tracks[track_index], eff_obj.tracks[track_index]);
                ++track_index;
            }
            ++object_index;
        }

        std::ostringstream new_axml_stream;
        adm::writeXml(new_axml_stream, document);
        const std::string new_axml = new_axml_stream.str();

        // chunk-level rewrite：仅替换 axml payload，data / chna / fmt / 其它 chunk
        // 的原始字节逐块复制（不解码 PCM），音频 bit-exact。采样率无关，96k/192k 同样支持。
        return rewrite_bwf_replacing_axml(src_path, dst_path, new_axml);
    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error,
                          std::string("写回 ADM 文件失败：") + e.what(),
                          "input=" + src_path + " output=" + dst_path);
    } catch (...) {
        return make_error(ErrorCode::io_error, "写回 ADM 文件时发生未知异常", "input=" + src_path);
    }
}

} // namespace mradm::io
