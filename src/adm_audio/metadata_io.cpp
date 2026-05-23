#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/errors.h"

#include "audio_io_internal.h"

namespace mradm::audio {

std::string compact_metadata_comment(const MetadataFields& meta) {
    std::string comments;
    const auto append = [&](std::string_view kv) {
        if (!comments.empty()) {
            comments += ' ';
        }
        comments.append(kv);
    };
    if (!meta.renderer.empty()) {
        append("renderer=" + meta.renderer);
    }
    if (!meta.output_layout.empty()) {
        append("layout=" + meta.output_layout);
    }
    if (meta.lufs) {
        append(fmt::format("loudness={:.1f}LUFS", *meta.lufs));
    }
    if (meta.peak_dbtp) {
        append(fmt::format("peak={:.2f}dBTP", *meta.peak_dbtp));
    }
    return comments;
}

namespace {

using Mp4Buf = std::vector<uint8_t>;
using Mp4FourCC = std::array<uint8_t, 4>;

struct Mp4Atom {
    std::size_t offset{};
    std::size_t size{};
    std::size_t header_size{};
    Mp4FourCC type{};
};

[[nodiscard]] Mp4FourCC mp4_fourcc(std::string_view text) {
    Mp4FourCC out{};
    const auto n = std::min(out.size(), text.size());
    std::transform(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(n), out.begin(), [](char c) {
        return static_cast<uint8_t>(c);
    });
    return out;
}

[[nodiscard]] bool mp4_type_is(const Mp4FourCC& type, std::string_view text) {
    return type == mp4_fourcc(text);
}

[[nodiscard]] uint32_t mp4_read_u32(const Mp4Buf& data, std::size_t off) {
    return (static_cast<uint32_t>(data[off]) << 24U) | (static_cast<uint32_t>(data[off + 1U]) << 16U) |
           (static_cast<uint32_t>(data[off + 2U]) << 8U) | data[off + 3U];
}

[[nodiscard]] uint64_t mp4_read_u64(const Mp4Buf& data, std::size_t off) {
    uint64_t out = 0;
    for (std::size_t i = 0; i < 8U; ++i) {
        out = (out << 8U) | data[off + i];
    }
    return out;
}

void mp4_append_u32(Mp4Buf& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24U));
    out.push_back(static_cast<uint8_t>(value >> 16U));
    out.push_back(static_cast<uint8_t>(value >> 8U));
    out.push_back(static_cast<uint8_t>(value));
}

void mp4_append_fourcc(Mp4Buf& out, const Mp4FourCC& type) {
    out.insert(out.end(), type.begin(), type.end());
}

void mp4_append_atom(Mp4Buf& out, const Mp4FourCC& type, const Mp4Buf& payload) {
    mp4_append_u32(out, static_cast<uint32_t>(payload.size() + 8U));
    mp4_append_fourcc(out, type);
    out.insert(out.end(), payload.begin(), payload.end());
}

[[nodiscard]] std::optional<Mp4Atom> mp4_next_atom(const Mp4Buf& data, std::size_t pos, std::size_t limit) {
    if (pos + 8U > limit || limit > data.size()) {
        return std::nullopt;
    }

    uint64_t size = mp4_read_u32(data, pos);
    std::size_t header_size = 8U;
    if (size == 1U) {
        if (pos + 16U > limit) {
            return std::nullopt;
        }
        size = mp4_read_u64(data, pos + 8U);
        header_size = 16U;
    } else if (size == 0U) {
        size = limit - pos;
    }
    if (size < header_size || size > static_cast<uint64_t>(limit - pos) ||
        size > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return std::nullopt;
    }

    Mp4Atom atom;
    atom.offset = pos;
    atom.size = static_cast<std::size_t>(size);
    atom.header_size = header_size;
    std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(pos + 4U), atom.type.size(), atom.type.begin());
    return atom;
}

[[nodiscard]] Mp4Buf mp4_text_data_atom(std::string_view value) {
    Mp4Buf payload;
    mp4_append_u32(payload, 1U); // UTF-8 text data type
    mp4_append_u32(payload, 0U); // locale
    payload.insert(payload.end(), value.begin(), value.end());

    Mp4Buf out;
    mp4_append_atom(out, mp4_fourcc("data"), payload);
    return out;
}

[[nodiscard]] Mp4Buf mp4_text_item(const Mp4FourCC& item_type, std::string_view value) {
    Mp4Buf item;
    const auto data_atom = mp4_text_data_atom(value);
    item.insert(item.end(), data_atom.begin(), data_atom.end());

    Mp4Buf out;
    mp4_append_atom(out, item_type, item);
    return out;
}

[[nodiscard]] Mp4Buf build_mp4_metadata_udta(const MetadataFields& meta) {
    Mp4Buf ilst_payload;
    const auto append_item = [&](const Mp4FourCC& type, const std::string& value) {
        if (value.empty()) {
            return;
        }
        auto item = mp4_text_item(type, value);
        ilst_payload.insert(ilst_payload.end(), item.begin(), item.end());
    };

    append_item(mp4_fourcc("\xA9"
                           "too"),
                meta.encoder);
    append_item(mp4_fourcc("\xA9"
                           "day"),
                meta.date_utc);
    append_item(mp4_fourcc("\xA9"
                           "cmt"),
                compact_metadata_comment(meta));
    if (ilst_payload.empty()) {
        return {};
    }

    Mp4Buf ilst;
    mp4_append_atom(ilst, mp4_fourcc("ilst"), ilst_payload);

    Mp4Buf hdlr_payload;
    mp4_append_u32(hdlr_payload, 0U); // version + flags
    mp4_append_u32(hdlr_payload, 0U); // pre-defined
    mp4_append_fourcc(hdlr_payload, mp4_fourcc("mdir"));
    mp4_append_fourcc(hdlr_payload, mp4_fourcc("appl"));
    mp4_append_u32(hdlr_payload, 0U);
    mp4_append_u32(hdlr_payload, 0U);
    hdlr_payload.push_back(0U); // empty name

    Mp4Buf meta_payload;
    mp4_append_u32(meta_payload, 0U); // FullBox version + flags
    Mp4Buf hdlr;
    mp4_append_atom(hdlr, mp4_fourcc("hdlr"), hdlr_payload);
    meta_payload.insert(meta_payload.end(), hdlr.begin(), hdlr.end());
    meta_payload.insert(meta_payload.end(), ilst.begin(), ilst.end());

    Mp4Buf meta_atom;
    mp4_append_atom(meta_atom, mp4_fourcc("meta"), meta_payload);

    Mp4Buf udta_payload;
    udta_payload.insert(udta_payload.end(), meta_atom.begin(), meta_atom.end());

    Mp4Buf udta;
    mp4_append_atom(udta, mp4_fourcc("udta"), udta_payload);
    return udta;
}

Result<void> write_mp4_metadata(const std::string& path, const MetadataFields& meta) {
    const auto udta = build_mp4_metadata_udta(meta);
    if (udta.empty()) {
        return {};
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return make_error(ErrorCode::io_error, "cannot open MP4 for metadata read", "path=" + path);
    }
    Mp4Buf data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return make_error(ErrorCode::io_error, "failed to read MP4 for metadata update", "path=" + path);
    }

    std::optional<Mp4Atom> moov;
    std::optional<Mp4Atom> following_free;
    std::size_t pos = 0;
    while (pos < data.size()) {
        auto atom = mp4_next_atom(data, pos, data.size());
        if (!atom) {
            return make_error(ErrorCode::io_error, "malformed MP4 atom table", "path=" + path);
        }
        if (moov && atom->offset == moov->offset + moov->size && mp4_type_is(atom->type, "free")) {
            following_free = atom;
            break;
        }
        if (mp4_type_is(atom->type, "moov")) {
            moov = atom;
        }
        pos = atom->offset + atom->size;
    }
    if (!moov) {
        return make_error(ErrorCode::io_error, "MP4 metadata: missing moov atom", "path=" + path);
    }
    if (moov->header_size != 8U || moov->size + udta.size() > std::numeric_limits<uint32_t>::max()) {
        return make_error(ErrorCode::unsupported, "MP4 metadata: unsupported large moov atom", "path=" + path);
    }

    const std::size_t insert_size = udta.size();
    const bool moov_at_eof = moov->offset + moov->size == data.size();
    if (!moov_at_eof &&
        (!following_free || following_free->header_size != 8U || following_free->size < insert_size + 8U)) {
        return make_error(ErrorCode::unsupported,
                          "MP4 metadata: no adjacent free atom large enough for safe in-place moov growth",
                          "path=" + path);
    }

    Mp4Buf out;
    out.reserve(data.size() + (moov_at_eof ? insert_size : 0U));
    out.insert(out.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(moov->offset));
    mp4_append_u32(out, static_cast<uint32_t>(moov->size + insert_size));
    out.insert(out.end(),
               data.begin() + static_cast<std::ptrdiff_t>(moov->offset + 4U),
               data.begin() + static_cast<std::ptrdiff_t>(moov->offset + moov->size));
    out.insert(out.end(), udta.begin(), udta.end());

    if (!moov_at_eof) {
        const std::size_t new_free_size = following_free->size - insert_size;
        mp4_append_u32(out, static_cast<uint32_t>(new_free_size));
        out.insert(out.end(),
                   data.begin() + static_cast<std::ptrdiff_t>(following_free->offset + 4U),
                   data.begin() + static_cast<std::ptrdiff_t>(following_free->offset + 8U));
        out.insert(out.end(),
                   data.begin() + static_cast<std::ptrdiff_t>(following_free->offset + 8U),
                   data.begin() + static_cast<std::ptrdiff_t>(following_free->offset + new_free_size));
        out.insert(out.end(),
                   data.begin() + static_cast<std::ptrdiff_t>(following_free->offset + following_free->size),
                   data.end());
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return make_error(ErrorCode::io_error, "cannot open MP4 for metadata rewrite", "path=" + path);
    }
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!file) {
        return make_error(ErrorCode::io_error, "failed to rewrite MP4 metadata", "path=" + path);
    }
    return {};
}

} // namespace

Result<void> write_file_metadata(const std::string& path, const MetadataFields& meta) {
    auto ext = std::filesystem::path(path).extension().string();
    std::ranges::transform(
        ext, ext.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    if (ext == ".caf") {
        return write_caf_metadata(path, meta);
    }
    if (ext == ".wav") {
        return write_wav_metadata(path, meta);
    }
    if (ext == ".flac") {
        return write_flac_metadata(path, meta);
    }
    if (ext == ".mka") {
        return write_mka_metadata(path, meta);
    }
    if (ext == ".m4a" || ext == ".mp4") {
        return write_mp4_metadata(path, meta);
    }
    return {}; // unsupported extension — silently skip
}

} // namespace mradm::audio
