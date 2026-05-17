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

std::vector<SceneObject> extract_objects(const std::shared_ptr<adm::Document>& doc,
                                         const std::map<std::string, uint16_t>& uid_map) {
    std::vector<SceneObject> result;
    for (const auto& obj : doc->getElements<adm::AudioObject>()) {
        SceneObject out;
        out.id = adm::formatId(obj->get<adm::AudioObjectId>());
        if (obj->has<adm::AudioObjectName>()) {
            out.name = obj->get<adm::AudioObjectName>().get();
        }
        for (const auto& uid : obj->getReferences<adm::AudioTrackUid>()) {
            SceneTrackRef ref;
            ref.track_uid = adm::formatId(uid->get<adm::AudioTrackUidId>());
            auto it = uid_map.find(ref.track_uid);
            if (it != uid_map.end()) {
                ref.channel_index = it->second;
            }
            // channel_index remains nullopt if UID has no CHNA entry
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
        scene.objects = extract_objects(document, uid_map);

        return scene;

    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error, std::string("解析 BW64/ADM 文件失败：") + e.what(), "input=" + path);
    } catch (...) {
        return make_error(ErrorCode::io_error, "解析 BW64/ADM 文件时发生未知异常", "input=" + path);
    }
}

} // namespace mradm::io
