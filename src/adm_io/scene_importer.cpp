#include <algorithm>
#include <cctype>
#include <chrono>
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

#include "adm/audio_io.h"
#include "adm/io.h"

#include "metadata.h"

namespace mradm::io {

namespace {

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

struct WaveAdmMetadata {
    std::string axml;
    std::map<std::string, uint16_t> uid_to_channel;
};

// AXML and CHNA come from the Rust container parser; the CHNA -> channel policy stays here.
[[nodiscard]] Result<WaveAdmMetadata> read_wave_adm_metadata(const std::string& path) {
    auto chunks = audio::read_wav_adm_chunks(path);
    if (!chunks) {
        auto error = chunks.error();
        error.context = "input=" + path;
        return tl::unexpected{std::move(error)};
    }
    WaveAdmMetadata metadata;
    metadata.axml = std::move(chunks->axml);
    for (auto& record : chunks->chna) {
        if (record.track_index == 0U) {
            return make_error(ErrorCode::io_error, "CHNA trackIndex 必须从 1 开始", "input=" + path);
        }
        auto uid = normalize_uid(std::move(record.uid));
        if (!uid.empty()) {
            metadata.uid_to_channel[std::move(uid)] = static_cast<uint16_t>(record.track_index - 1U);
        }
    }
    return metadata;
}

} // namespace

Result<AdmScene> import_scene(const std::string& path) {
    try {
        auto reader_res = audio::FloatWavReader::open(path);
        if (!reader_res) {
            return tl::unexpected{reader_res.error()};
        }
        const auto& reader = *reader_res;
        if (reader.channels() > std::numeric_limits<uint16_t>::max()) {
            return make_error(ErrorCode::unsupported, "ADM WAVE 声道数超过 65535", "input=" + path);
        }
        auto metadata_res = read_wave_adm_metadata(path);
        if (!metadata_res) {
            return tl::unexpected{metadata_res.error()};
        }
        auto& metadata = *metadata_res;

        SceneInfo info;
        info.file_path = path;
        info.sample_rate = reader.sample_rate();
        info.num_channels = static_cast<uint16_t>(reader.channels());
        info.num_frames = reader.frame_count();

        if (metadata.axml.empty()) {
            return make_error(ErrorCode::io_error, "axml chunk 缺失或为空", "input=" + path);
        }

        auto scene = metadata::import_axml(metadata.axml, metadata.uid_to_channel, reader.sample_rate());
        if (!scene) {
            auto error = scene.error();
            error.context = "input=" + path;
            return tl::unexpected{std::move(error)};
        }
        scene->info = std::move(info);
        return scene;

    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error, std::string("解析 BW64/ADM 文件失败：") + e.what(), "input=" + path);
    } catch (...) {
        return make_error(ErrorCode::io_error, "解析 BW64/ADM 文件时发生未知异常", "input=" + path);
    }
}

Result<std::string> get_axml(const std::string& path) {
    try {
        auto metadata = read_wave_adm_metadata(path);
        if (!metadata) {
            return tl::unexpected{metadata.error()};
        }
        if (metadata->axml.empty()) {
            return make_error(ErrorCode::io_error, "axml chunk 缺失或为空", "input=" + path);
        }
        return std::move(metadata->axml);
    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error, std::string("读取 AXML 失败：") + e.what(), "input=" + path);
    }
}

namespace {

// ---- chunk-level BWF rewrite (bit-exact PCM) ----
// mradm-wav 只替换 axml chunk 的 payload，其余 chunk（data / chna / fmt / bext / JUNK / 未知）
// 的原始字节逐块复制，绝不解码 / 重编码 PCM —— 这是音频真正 bit-exact 的唯一方式。

// Rewrite src_path → dst_path through a temporary file so a failure never corrupts the destination
// (or the source when dst == src).
Result<void>
rewrite_bwf_replacing_axml(const std::string& src_path, const std::string& dst_path, const std::string& new_axml) {
    auto written = audio::replace_wav_axml(src_path, dst_path, new_axml);
    if (!written) {
        auto error = written.error();
        error.context = "input=" + src_path + " output=" + dst_path;
        return tl::unexpected{std::move(error)};
    }
    return {};
}

} // namespace

Result<void> write_scene(const std::string& src_path,
                         const AdmScene& original,
                         const AdmScene& effective,
                         const std::string& dst_path) {
    try {
        auto xml_res = get_axml(src_path);
        if (!xml_res) {
            return tl::unexpected{xml_res.error()};
        }
        auto patched = metadata::patch_axml(*xml_res, original, effective);
        if (!patched) {
            auto error = patched.error();
            error.context = "input=" + src_path + " output=" + dst_path;
            return tl::unexpected{std::move(error)};
        }
        return rewrite_bwf_replacing_axml(src_path, dst_path, *patched);

    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error,
                          std::string("写回 ADM 文件失败：") + e.what(),
                          "input=" + src_path + " output=" + dst_path);
    } catch (...) {
        return make_error(ErrorCode::io_error, "写回 ADM 文件时发生未知异常", "input=" + src_path);
    }
}

} // namespace mradm::io
