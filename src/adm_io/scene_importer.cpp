#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
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

[[nodiscard]] uint16_t adm_chunk_read_u16(const char* data) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(data);
    return static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8U);
}

[[nodiscard]] uint32_t adm_chunk_read_u32(const char* data) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(data);
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8U) |
           (static_cast<uint32_t>(bytes[2]) << 16U) | (static_cast<uint32_t>(bytes[3]) << 24U);
}

[[nodiscard]] uint64_t adm_chunk_read_u64(const char* data) {
    return static_cast<uint64_t>(adm_chunk_read_u32(data)) |
           (static_cast<uint64_t>(adm_chunk_read_u32(data + 4)) << 32U);
}

struct WaveAdmMetadata {
    std::string axml;
    std::map<std::string, uint16_t> uid_to_channel;
};

// NOLINTNEXTLINE(readability-function-size): bounded RIFF scanning and both ADM chunks share one cursor/state machine.
[[nodiscard]] Result<WaveAdmMetadata> read_wave_adm_metadata(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return make_error(ErrorCode::io_error, "无法打开 WAVE 文件", "input=" + path);
    }
    std::array<char, 12> header{};
    input.read(header.data(), static_cast<std::streamsize>(header.size()));
    const std::string_view container{header.data(), 4U};
    if (!input || std::string_view{header.data() + 8, 4U} != "WAVE" ||
        (container != "RIFF" && container != "RF64" && container != "BW64")) {
        return make_error(ErrorCode::io_error, "不是有效的 RIFF/RF64/BW64 WAVE 文件", "input=" + path);
    }

    input.seekg(0, std::ios::end);
    const auto end_position = input.tellg();
    if (end_position < 0) {
        return make_error(ErrorCode::io_error, "无法读取 WAVE 文件大小", "input=" + path);
    }
    const auto file_size = static_cast<uint64_t>(end_position);
    uint64_t data_size64 = 0U;
    WaveAdmMetadata metadata;
    uint64_t position = 12U;
    while (position + 8U <= file_size) {
        std::array<char, 8> chunk_header{};
        input.seekg(static_cast<std::streamoff>(position), std::ios::beg);
        input.read(chunk_header.data(), static_cast<std::streamsize>(chunk_header.size()));
        if (!input) {
            return make_error(ErrorCode::io_error, "读取 WAVE chunk 头失败", "input=" + path);
        }
        const std::string_view id{chunk_header.data(), 4U};
        const uint32_t size32 = adm_chunk_read_u32(chunk_header.data() + 4);
        const uint64_t payload_offset = position + 8U;
        uint64_t payload_size = size32;
        if (id == "ds64") {
            if (size32 < 28U || payload_offset + 28U > file_size) {
                return make_error(ErrorCode::io_error, "无效的 ds64 chunk", "input=" + path);
            }
            std::array<char, 28> ds64{};
            input.read(ds64.data(), static_cast<std::streamsize>(ds64.size()));
            if (!input) {
                return make_error(ErrorCode::io_error, "读取 ds64 chunk 失败", "input=" + path);
            }
            data_size64 = adm_chunk_read_u64(ds64.data() + 8);
        } else if (id == "data" && size32 == std::numeric_limits<uint32_t>::max()) {
            if (data_size64 == 0U) {
                return make_error(ErrorCode::io_error, "RF64/BW64 data chunk 缺少 ds64 大小", "input=" + path);
            }
            payload_size = data_size64;
        }
        if (payload_offset > file_size || payload_size > file_size - payload_offset) {
            return make_error(ErrorCode::io_error, "WAVE chunk 超出文件边界", "input=" + path);
        }

        if (id == "axml") {
            const auto max_axml_size =
                std::min<uint64_t>(metadata.axml.max_size(), std::numeric_limits<std::streamsize>::max());
            if (payload_size > max_axml_size) {
                return make_error(ErrorCode::unsupported, "AXML chunk 过大", "input=" + path);
            }
            metadata.axml.resize(static_cast<std::size_t>(payload_size));
            input.seekg(static_cast<std::streamoff>(payload_offset), std::ios::beg);
            input.read(metadata.axml.data(), static_cast<std::streamsize>(payload_size));
            if (!input) {
                return make_error(ErrorCode::io_error, "读取 AXML chunk 失败", "input=" + path);
            }
        } else if (id == "chna") {
            const std::vector<char> empty_payload;
            const auto max_chna_size =
                std::min<uint64_t>(empty_payload.max_size(), std::numeric_limits<std::streamsize>::max());
            if (payload_size < 4U || payload_size > max_chna_size) {
                return make_error(ErrorCode::io_error, "无效的 CHNA chunk", "input=" + path);
            }
            std::vector<char> payload(static_cast<std::size_t>(payload_size));
            input.seekg(static_cast<std::streamoff>(payload_offset), std::ios::beg);
            input.read(payload.data(), static_cast<std::streamsize>(payload_size));
            if (!input) {
                return make_error(ErrorCode::io_error, "读取 CHNA chunk 失败", "input=" + path);
            }
            const uint16_t uid_count = adm_chunk_read_u16(payload.data() + 2);
            const uint64_t expected_size = 4U + (static_cast<uint64_t>(uid_count) * 40U);
            if (payload_size < expected_size) {
                return make_error(ErrorCode::io_error, "CHNA UID 表被截断", "input=" + path);
            }
            for (uint16_t index = 0U; index < uid_count; ++index) {
                const std::size_t offset = 4U + (static_cast<std::size_t>(index) * 40U);
                const uint16_t track_index = adm_chunk_read_u16(payload.data() + offset);
                if (track_index == 0U) {
                    return make_error(ErrorCode::io_error, "CHNA trackIndex 必须从 1 开始", "input=" + path);
                }
                auto uid = normalize_uid(std::string{payload.data() + offset + 2U, 12U});
                if (!uid.empty()) {
                    metadata.uid_to_channel[std::move(uid)] = static_cast<uint16_t>(track_index - 1U);
                }
            }
        }

        const uint64_t padded_size = payload_size + (payload_size & 1U);
        if (payload_offset > std::numeric_limits<uint64_t>::max() - padded_size) {
            return make_error(ErrorCode::io_error, "WAVE chunk 表溢出", "input=" + path);
        }
        const uint64_t next = payload_offset + padded_size;
        if (next <= position) {
            return make_error(ErrorCode::io_error, "无效的 WAVE chunk 表", "input=" + path);
        }
        position = next;
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
