#include <cstdint>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

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

// AudioId::uid() returns a 12-byte fixed-width string padded with spaces
// (and possibly NUL bytes). Trim both to recover the actual UID.
std::string trim_uid(std::string raw) {
    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\0')) {
        raw.pop_back();
    }
    return raw;
}

// Build UID string → 0-based channel index from the CHNA chunk.
std::map<std::string, uint16_t> make_uid_map(const std::shared_ptr<bw64::ChnaChunk>& chna) {
    std::map<std::string, uint16_t> result;
    if (!chna) {
        return result;
    }
    for (const auto& entry : chna->audioIds()) {
        std::string uid = trim_uid(entry.uid());
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
        block.jump_position = adm::isEnabled(const_cast<adm::JumpPosition&>(jp));
        if (jp.has<adm::InterpolationLength>()) {
            block.interp_length_samples =
                time_to_samples(adm::Time{jp.get<adm::InterpolationLength>().get()}, sample_rate);
        }

        ref.blocks.push_back(block);
    }
}

void append_direct_speakers_blocks_from_cf(const std::shared_ptr<adm::AudioChannelFormat>& cf,
                                           const std::shared_ptr<adm::AudioPackFormat>& pf,
                                           SceneTrackRef& ref) {
    const auto raw_blocks = cf->getElements<adm::AudioBlockFormatDirectSpeakers>();
    for (const auto& raw : raw_blocks) {
        SceneDirectSpeakersBlock block;

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
        }

        if (raw.has<adm::Gain>()) {
            block.gain = static_cast<float>(raw.get<adm::Gain>().get());
        }

        ref.ds_blocks.push_back(std::move(block));
    }
}

void populate_track_blocks(const std::shared_ptr<adm::AudioTrackUid>& uid,
                           SceneTrackRef& ref,
                           uint32_t sample_rate,
                           uint64_t obj_start_sample) {
    const auto pf = uid->getReference<adm::AudioPackFormat>();
    if (!pf) {
        return;
    }

    for (const auto& cf : pf->getReferences<adm::AudioChannelFormat>()) {
        const auto type = cf->get<adm::TypeDescriptor>();
        if (type == adm::TypeDefinition::OBJECTS) {
            append_objects_blocks_from_cf(cf, ref, sample_rate, obj_start_sample);
        } else if (type == adm::TypeDefinition::DIRECT_SPEAKERS) {
            append_direct_speakers_blocks_from_cf(cf, pf, ref);
        }
    }
}

std::vector<SceneObject> extract_objects(const std::shared_ptr<adm::Document>& doc,
                                         const std::map<std::string, uint16_t>& uid_map,
                                         uint32_t sample_rate) {
    std::vector<SceneObject> result;
    for (const auto& obj : doc->getElements<adm::AudioObject>()) {
        SceneObject out;
        out.id = adm::formatId(obj->get<adm::AudioObjectId>());
        if (obj->has<adm::AudioObjectName>()) {
            out.name = obj->get<adm::AudioObjectName>().get();
        }
        // AudioObject.start is DefaultParameter — always present, default 0.
        const uint64_t obj_start = time_to_samples(obj->get<adm::Start>().get(), sample_rate);
        for (const auto& uid : obj->getReferences<adm::AudioTrackUid>()) {
            SceneTrackRef ref;
            ref.track_uid = adm::formatId(uid->get<adm::AudioTrackUidId>());
            auto it = uid_map.find(ref.track_uid);
            if (it != uid_map.end()) {
                ref.channel_index = it->second;
            }
            populate_track_blocks(uid, ref, sample_rate, obj_start);
            out.tracks.push_back(std::move(ref));
        }
        result.push_back(std::move(out));
    }
    return result;
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
        result.push_back(std::move(out));
    }
    return result;
}

std::vector<SceneProgramme> extract_programmes(const std::shared_ptr<adm::Document>& doc) {
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
        result.push_back(std::move(out));
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
        scene.programmes = extract_programmes(document);
        scene.contents = extract_contents(document);
        scene.objects = extract_objects(document, uid_map, reader->sampleRate());

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
