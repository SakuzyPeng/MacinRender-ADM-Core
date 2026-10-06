// WAVE container metadata parity: every case runs the frozen C++ implementation and the mradm-wav
// path on separate copies of the same fixture and requires identical bytes (or the same error code).
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "adm/audio_io.h"
#include "adm/io.h"

#include "../reference/wav_container/legacy.h"
#include "audio_io_internal.h"

namespace {

namespace fs = std::filesystem;
namespace legacy = mradm::wav_container_legacy;
using Bytes = std::vector<uint8_t>;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Bytes load(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void save(const fs::path& path, const Bytes& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(output.good(), "fixture write failed: " + path.string());
}

void put16(Bytes& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8U));
}
void put32(Bytes& out, uint32_t value) {
    put16(out, value & 0xFFFFU);
    put16(out, value >> 16U);
}
void put64(Bytes& out, uint64_t value) {
    put32(out, static_cast<uint32_t>(value));
    put32(out, static_cast<uint32_t>(value >> 32U));
}
void put_text(Bytes& out, std::string_view text) {
    out.insert(out.end(), text.begin(), text.end());
}

struct Chunk {
    std::string id;
    Bytes payload;
};

Bytes fmt_payload(uint16_t tag, uint16_t channels, uint16_t bits, std::optional<uint32_t> mask = std::nullopt) {
    const uint32_t align = channels * bits / 8U;
    Bytes fmt;
    put16(fmt, mask ? 0xFFFEU : tag);
    put16(fmt, channels);
    put32(fmt, 48000U);
    put32(fmt, 48000U * align);
    put16(fmt, align);
    put16(fmt, bits);
    if (mask) {
        put16(fmt, 22U);
        put16(fmt, bits);
        put32(fmt, *mask);
        put16(fmt, tag);
        const std::array<uint8_t, 14> guid_tail{0, 0, 0, 0, 16, 0, 128, 0, 0, 170, 0, 56, 155, 113};
        fmt.insert(fmt.end(), guid_tail.begin(), guid_tail.end());
    }
    return fmt;
}

Bytes samples(std::size_t size, uint32_t seed) {
    Bytes data(size);
    for (auto& byte : data) {
        seed = (seed * 1664525U) + 1013904223U;
        byte = static_cast<uint8_t>(seed >> 24U);
    }
    return data;
}

// RF64/BW64 get a ds64 first and a sentinel data size; `table` adds ds64 size-table entries.
Bytes wave(std::string_view container, const std::vector<Chunk>& chunks, uint32_t table = 0) {
    const bool wide = container != "RIFF";
    Bytes body;
    uint64_t data_size = 0;
    for (const auto& chunk : chunks) {
        put_text(body, chunk.id);
        const bool sentinel = wide && chunk.id == "data";
        put32(body, sentinel ? 0xFFFFFFFFU : static_cast<uint32_t>(chunk.payload.size()));
        body.insert(body.end(), chunk.payload.begin(), chunk.payload.end());
        if ((chunk.payload.size() & 1U) != 0U) {
            body.push_back(0);
        }
        if (chunk.id == "data") {
            data_size = chunk.payload.size();
        }
    }
    const uint64_t ds64_disk = wide ? 8U + 28U + (12U * table) : 0U;
    const uint64_t riff_size = 4U + ds64_disk + body.size();
    Bytes out;
    put_text(out, container);
    put32(out, wide ? 0xFFFFFFFFU : static_cast<uint32_t>(riff_size));
    put_text(out, "WAVE");
    if (wide) {
        put_text(out, "ds64");
        put32(out, 28U + (12U * table));
        put64(out, riff_size);
        put64(out, data_size);
        put64(out, 0U);
        put32(out, table);
        for (uint32_t entry = 0; entry < table; ++entry) {
            put_text(out, "JUNK");
            put64(out, 2U);
        }
    }
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

Bytes chna_payload(uint16_t tracks, const std::vector<std::pair<uint16_t, std::string>>& uids, uint16_t reserved = 0) {
    Bytes out;
    put16(out, tracks);
    put16(out, static_cast<uint32_t>(uids.size()));
    for (const auto& [track, uid] : uids) {
        put16(out, track);
        std::string padded = uid;
        padded.resize(12U, ' ');
        put_text(out, padded);
        put_text(out, "AT_00010001_01");
        put_text(out, "AP_00010002");
        out.push_back(0);
    }
    out.resize(out.size() + (std::size_t{40} * reserved), 0);
    return out;
}

std::string axml_text(std::size_t size) {
    std::string xml = "<?xml version=\"1.0\"?><ebuCoreMain>";
    while (xml.size() + 14U < size) {
        xml += "<a/>";
    }
    return xml + "</ebuCoreMain>";
}

Bytes text_bytes(const std::string& text) {
    return {text.begin(), text.end()};
}

struct Fixture {
    std::string name;
    Bytes bytes;
    uint16_t channels{2};
    bool audio_supported{true};
};

std::vector<Fixture> fixtures() {
    std::vector<Fixture> out;
    const auto data24 = samples(std::size_t{2} * 3U * 517U, 1U);
    const auto data16 = samples(std::size_t{2} * 333U, 2U);
    const auto data_f32 = samples(std::size_t{2} * 4U * 211U, 3U);
    const auto data24_mono_odd = samples(std::size_t{3} * 335U, 4U); // odd byte count
    const auto axml = text_bytes(axml_text(301U));
    const auto chna = chna_payload(2, {{1, "ATU_00000001"}, {2, "atu_00000002"}});
    out.push_back({"riff-empty", wave("RIFF", {{"fmt ", fmt_payload(1, 2, 24)}, {"data", {}}})});
    out.push_back({"riff-pcm24", wave("RIFF", {{"fmt ", fmt_payload(1, 2, 24)}, {"data", data24}})});
    out.push_back(
        {"riff-pcm16-junk-list",
         wave("RIFF",
              {{"JUNK", Bytes(28, 0)}, {"fmt ", fmt_payload(1, 1, 16)}, {"LIST", text_bytes("odd")}, {"data", data16}}),
         1});
    out.push_back(
        {"riff-pcm24-odd-data", wave("RIFF", {{"fmt ", fmt_payload(1, 1, 24)}, {"data", data24_mono_odd}}), 1});
    out.push_back({"rf64-float-fact",
                   wave("RF64", {{"fmt ", fmt_payload(3, 2, 32)}, {"fact", Bytes(4, 0)}, {"data", data_f32}})});
    out.push_back(
        {"bw64-pcm24-adm",
         wave("BW64", {{"fmt ", fmt_payload(1, 2, 24, 0U)}, {"chna", chna}, {"data", data24}, {"axml", axml}})});
    out.push_back({"riff-pcm32-adm-ext",
                   wave("RIFF",
                        {{"fmt ", fmt_payload(1, 2, 32, 0x3U)},
                         {"chna", chna_payload(2, {{1, "ATU_00000001"}, {2, "ATU_00000002"}}, 2)},
                         {"axml", text_bytes(axml_text(77U))},
                         {"data", samples(std::size_t{2} * 4U * 64U, 5U)}})});
    out.push_back({"riff-hoa3-ambi",
                   wave("RIFF",
                        {{"fmt ", fmt_payload(1, 16, 24)},
                         {"ambi", Bytes(16, 7)},
                         {"data", samples(std::size_t{16} * 3U * 40U, 6U)}}),
                   16});
    out.push_back({"riff-hoa3",
                   wave("RIFF", {{"fmt ", fmt_payload(3, 16, 32)}, {"data", samples(std::size_t{16} * 4U * 40U, 7U)}}),
                   16});
    out.push_back(
        {"rf64-pcm16-table",
         wave("RF64",
              {{"fmt ", fmt_payload(1, 2, 16)}, {"axml", axml}, {"data", samples(std::size_t{2} * 2U * 50U, 8U)}},
              1)});
    const auto mono_chna = chna_payload(1, {{1, "ATU_00000001"}});
    for (const std::string_view container : {"RIFF", "RF64"}) {
        out.push_back(
            {std::string{container} + "-pcm8-metadata",
             wave(container,
                  {{"fmt ", fmt_payload(1, 1, 8)}, {"chna", mono_chna}, {"data", samples(3, 9)}, {"axml", axml}}),
             1,
             false});
        out.push_back(
            {std::string{container} + "-float64-metadata",
             wave(
                 container,
                 {{"fmt ", fmt_payload(3, 1, 64, 0U)}, {"chna", mono_chna}, {"data", samples(24, 10)}, {"axml", axml}}),
             1,
             false});
    }
    return out;
}

using Outcome = std::pair<int, Bytes>; // error code (0 = ok), resulting file bytes

template <typename T> int code_of(const mradm::Result<T>& result) {
    return result ? 0 : static_cast<int>(result.error().code);
}

int& case_count() {
    static int count = 0;
    return count;
}

void expect_same(const std::string& name, const Outcome& expected, const Outcome& actual, bool compare_bytes = true) {
    require(expected.first == actual.first,
            name + ": error code differs (legacy " + std::to_string(expected.first) + ", rust " +
                std::to_string(actual.first) + ")");
    if (expected.first == 0 && compare_bytes) {
        require(expected.second == actual.second, name + ": output bytes differ");
    }
    ++case_count();
}

// Runs an in-place operation on two copies of `source`.
template <typename Operation> Outcome run_in_place(const fs::path& path, const Bytes& source, Operation op) {
    save(path, source);
    const int code = op(path.string());
    return {code, load(path)};
}

std::vector<std::pair<std::string, mradm::audio::WavLayoutFinalization>> layouts(uint16_t channels) {
    using mradm::audio::WavLayoutFinalization;
    std::vector<std::pair<std::string, WavLayoutFinalization>> out;
    out.emplace_back("identity", WavLayoutFinalization{});
    WavLayoutFinalization riff;
    riff.prefer_riff = true;
    out.emplace_back("prefer-riff", riff);
    WavLayoutFinalization forced;
    forced.force_extensible = true;
    forced.include_pcm_fact = true;
    out.emplace_back("force-extensible-fact", forced);
    if (channels == 2) {
        WavLayoutFinalization stereo;
        stereo.channel_mask = 0x3U;
        stereo.output_channel_sources = {1U, 0U};
        out.emplace_back("mask-swap", stereo);
        WavLayoutFinalization adm;
        adm.axml = axml_text(1001U);
        adm.chna = {{1, "ATU_00000001", "AT_00010001_01", "AP_00010002"}, {2, "ATU_2", "AT_00010002_01", "AP_1"}};
        out.emplace_back("adm", adm);
        WavLayoutFinalization adm_riff = adm;
        adm_riff.prefer_riff = true;
        adm_riff.axml += ' '; // odd AXML length
        out.emplace_back("adm-odd-riff", adm_riff);
        // Error cases: both implementations must reject them with the same code.
        WavLayoutFinalization bad_mask;
        bad_mask.channel_mask = 0x7U;
        out.emplace_back("bad-mask", bad_mask);
        WavLayoutFinalization bad_permutation;
        bad_permutation.output_channel_sources = {0U, 0U};
        out.emplace_back("bad-permutation", bad_permutation);
        WavLayoutFinalization short_permutation;
        short_permutation.output_channel_sources = {0U};
        out.emplace_back("short-permutation", short_permutation);
        WavLayoutFinalization axml_only;
        axml_only.axml = "<x/>";
        out.emplace_back("axml-without-chna", axml_only);
        WavLayoutFinalization bad_track = adm;
        bad_track.chna[1].track_index = 3;
        out.emplace_back("chna-track-range", bad_track);
        WavLayoutFinalization duplicate = adm;
        duplicate.chna[1].track_index = 1;
        out.emplace_back("chna-duplicate", duplicate);
        WavLayoutFinalization wide_uid = adm;
        wide_uid.chna[0].track_uid = "ATU_000000001";
        out.emplace_back("chna-width", wide_uid);
    }
    if (channels == 16) {
        WavLayoutFinalization reversed;
        for (uint16_t channel = 16; channel > 0; --channel) {
            reversed.output_channel_sources.push_back(static_cast<uint16_t>(channel - 1U));
        }
        reversed.force_extensible = true;
        out.emplace_back("reverse-16", reversed);
    }
    return out;
}

void finalize_parity(const fs::path& root, const Fixture& fixture) {
    for (const auto& item : layouts(fixture.channels)) {
        const auto& layout = item.second;
        const auto label = "finalize " + fixture.name + " " + item.first;
        const auto expected = run_in_place(root / "legacy.wav", fixture.bytes, [&](const std::string& path) {
            return code_of(legacy::finalize_wav_layout(path, layout));
        });
        const auto actual = run_in_place(root / "rust.wav", fixture.bytes, [&](const std::string& path) {
            return code_of(mradm::audio::finalize_wav_layout(path, layout));
        });
        expect_same(label, expected, actual);
    }
}

std::vector<std::pair<std::string, mradm::audio::MetadataFields>> metadata_cases() {
    using mradm::audio::MetadataFields;
    std::vector<std::pair<std::string, MetadataFields>> out;
    out.emplace_back("full",
                     MetadataFields{"MacinRender ADM Core", "2026-05-22T14:30:07Z", "ear", "0+2+0", -23.456, -1.004});
    out.emplace_back("empty", MetadataFields{});
    out.emplace_back("date-only", MetadataFields{"enc", "2026-05-22", "", "5.1", std::nullopt, 0.005});
    out.emplace_back("negative-half", MetadataFields{"e", "2026-05-22T00:00:00", "vbap", "x", -0.005, -24.125});
    out.emplace_back("long-fields",
                     MetadataFields{std::string(40, 'o'),
                                    "2026-05-22T14:30:07Z",
                                    std::string(300, 'r'),
                                    std::string(40, 'l'),
                                    12.0,
                                    std::nullopt});
    out.emplace_back("hoa3", MetadataFields{"enc", "2026-05-22T14:30:07Z", "hoa-encode", "hoa3", -18.0, -3.0});
    return out;
}

void metadata_parity(const fs::path& root, const Fixture& fixture) {
    for (const auto& item : metadata_cases()) {
        const auto& meta = item.second;
        const auto label = "metadata " + fixture.name + " " + item.first;
        // Apply twice so a second bext and an existing ambi are covered too.
        const auto expected = run_in_place(root / "legacy.wav", fixture.bytes, [&](const std::string& path) {
            const int first = code_of(legacy::write_wav_metadata(path, meta));
            return first != 0 ? first : code_of(legacy::write_wav_metadata(path, meta));
        });
        const auto actual = run_in_place(root / "rust.wav", fixture.bytes, [&](const std::string& path) {
            const int first = code_of(mradm::audio::write_wav_metadata(path, meta));
            return first != 0 ? first : code_of(mradm::audio::write_wav_metadata(path, meta));
        });
        expect_same(label, expected, actual);
    }
}

std::string normalize_uid(std::string raw) {
    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\0')) {
        raw.pop_back();
    }
    std::ranges::transform(raw, raw.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return raw;
}

void read_parity(const fs::path& root, const Fixture& fixture) {
    const auto path = root / "read.wav";
    save(path, fixture.bytes);
    const auto expected = legacy::read_wave_adm_metadata(path.string());
    const auto actual = mradm::audio::read_wav_adm_chunks(path.string());
    require(code_of(expected) == code_of(actual), "read " + fixture.name + ": error code differs");
    if (expected) {
        require(expected->axml == actual->axml, "read " + fixture.name + ": AXML differs");
        std::map<std::string, uint16_t> uid_to_channel;
        for (const auto& record : actual->chna) {
            require(record.track_index != 0U, "read " + fixture.name + ": unexpected track index 0");
            if (auto uid = normalize_uid(record.uid); !uid.empty()) {
                uid_to_channel[uid] = static_cast<uint16_t>(record.track_index - 1U);
            }
        }
        require(expected->uid_to_channel == uid_to_channel, "read " + fixture.name + ": CHNA map differs");
        if (!expected->axml.empty()) {
            const auto axml = mradm::io::get_axml(path.string());
            require(axml && *axml == expected->axml, "read " + fixture.name + ": get_axml differs");
        }
    }
    ++case_count();
}

void replace_parity(const fs::path& root, const Fixture& fixture) {
    for (const std::size_t size : {0U, 1U, 2U, 4097U}) {
        const auto label = "replace " + fixture.name + " " + std::to_string(size);
        const auto axml = axml_text(size);
        const auto source = root / "source.wav";
        save(source, fixture.bytes);
        const auto legacy_out = root / "legacy-out.wav";
        const auto rust_out = root / "rust-out.wav";
        const int expected = code_of(legacy::rewrite_bwf_replacing_axml(source.string(), legacy_out.string(), axml));
        const int actual = code_of(mradm::audio::replace_wav_axml(source.string(), rust_out.string(), axml));
        expect_same(label,
                    {expected, expected == 0 ? load(legacy_out) : Bytes{}},
                    {actual, actual == 0 ? load(rust_out) : Bytes{}});
        fs::remove(legacy_out);
        fs::remove(rust_out);
    }
}

void scan_case(const fs::path& path, const std::string& label) {
    const auto expected = legacy::scan_axml(path.string());
    const auto actual = mradm::audio::wav_has_chunk(path.string(), "axml");
    legacy::AxmlState state = legacy::AxmlState::invalid_wave;
    if (actual) {
        state = *actual ? legacy::AxmlState::present : legacy::AxmlState::absent;
    }
    require(expected == state, "scan " + label + ": routing differs");
    ++case_count();
}

void scan_parity(const fs::path& root, const std::vector<Fixture>& all) {
    const auto path = root / "scan.wav";
    for (const auto& fixture : all) {
        save(path, fixture.bytes);
        scan_case(path, fixture.name);
        // Truncated tails and trailing garbage end the tolerant scan instead of failing it.
        for (const std::size_t cut : {1U, 5U, 9U}) {
            auto truncated = fixture.bytes;
            truncated.resize(truncated.size() - cut);
            save(path, truncated);
            scan_case(path, fixture.name + " cut " + std::to_string(cut));
        }
        auto trailing = fixture.bytes;
        put_text(trailing, "axm");
        save(path, trailing);
        scan_case(path, fixture.name + " trailing");
    }
    const auto eight_bit = wave("RIFF", {{"fmt ", fmt_payload(1, 1, 8)}, {"data", samples(9, 9)}, {"axml", {'x'}}});
    save(path, eight_bit);
    scan_case(path, "8-bit with axml");
    save(path, Bytes{'R', 'I', 'F', 'F', 4, 0, 0, 0, 'A', 'I', 'F', 'F'});
    scan_case(path, "not wave");
    save(path, text_bytes("RIF"));
    scan_case(path, "short");
    scan_case(root / "missing.wav", "missing");
}

// Behaviour that intentionally differs from the frozen implementation.
void rust_only_checks(const fs::path& root, const std::vector<Fixture>& all) {
    const auto path = root / "rust-only.wav";
    // An invalid existing ambi is rejected before anything is written (the old code had already appended bext).
    const auto bad_ambi =
        wave("RIFF", {{"fmt ", fmt_payload(1, 16, 24)}, {"ambi", Bytes(12, 0)}, {"data", Bytes(48, 0)}});
    save(path, bad_ambi);
    mradm::audio::MetadataFields hoa;
    hoa.output_layout = "hoa3";
    require(!mradm::audio::write_wav_metadata(path.string(), hoa), "invalid ambi accepted");
    require(load(path) == bad_ambi, "invalid ambi left a partial bext");
    // Bytes beyond the declared container end are no longer absorbed into it.
    auto trailing = all.front().bytes;
    trailing.push_back(0);
    save(path, trailing);
    require(!mradm::audio::write_wav_metadata(path.string(), {}), "trailing garbage absorbed");
    require(load(path) == trailing, "rejected metadata write modified the file");
    // CHNA records may reserve unused space after numUIDs; track index 0 is reported for the caller.
    const auto reserved = wave(
        "RIFF",
        {{"fmt ", fmt_payload(1, 2, 16)}, {"chna", chna_payload(2, {{0, "ATU_00000009"}}, 3)}, {"data", Bytes(8, 0)}});
    save(path, reserved);
    const auto chunks = mradm::audio::read_wav_adm_chunks(path.string());
    require(chunks && chunks->axml.empty() && chunks->chna.size() == 1U && chunks->chna[0].track_index == 0U &&
                chunks->chna[0].uid == "ATU_00000009",
            "reserved CHNA records or track index 0 not reported");
    require(!mradm::io::get_axml(path.string()), "get_axml accepted a file without AXML");
    // A failed layout rewrite leaves neither the original modified nor a temporary file behind.
    const auto& source = all.front().bytes;
    save(path, source);
    mradm::audio::WavLayoutFinalization bad;
    bad.channel_mask = 0x1U;
    require(!mradm::audio::finalize_wav_layout(path.string(), bad), "bad mask accepted");
    require(load(path) == source, "failed finalize modified the source");
    std::stop_source stop;
    stop.request_stop();
    require(code_of(mradm::audio::finalize_wav_layout(path.string(), {}, stop.get_token())) ==
                static_cast<int>(mradm::ErrorCode::cancelled),
            "cancelled finalize did not report cancellation");
    const bool leftovers = std::ranges::any_of(fs::directory_iterator(root), [](const fs::directory_entry& entry) {
        return entry.path().filename().string().find("layout_") != std::string::npos;
    });
    require(!leftovers, "layout temporary file left behind");
}

void replace_alias_checks(const fs::path& root) {
    const auto source = root / "alias-source.wav";
    const auto destination = root / "alias-destination.wav";
    const auto format = fmt_payload(1, 1, 16);
    const auto audio = samples(20, 11);
    const auto original = wave("RIFF", {{"fmt ", format}, {"data", audio}, {"axml", text_bytes("<old/>")}});
    const auto expected = wave("RIFF", {{"fmt ", format}, {"data", audio}, {"axml", text_bytes("<new/>!")}});
    save(source, original);
    auto rewritten = mradm::audio::replace_wav_axml(source.string(), source.string(), "<new/>!");
    require(rewritten.has_value() && load(source) == expected, "in-place AXML replacement lost source data");

    save(source, original);
    save(destination, text_bytes("existing output"));
    rewritten = mradm::audio::replace_wav_axml(source.string(), destination.string(), "<new/>!");
    require(rewritten.has_value() && load(destination) == expected && load(source) == original,
            "replacing an existing output changed the source");
    fs::remove(destination);
    fs::create_hard_link(source, destination);
    rewritten = mradm::audio::replace_wav_axml(source.string(), destination.string(), "<new/>!");
    require(rewritten.has_value() && load(destination) == expected && load(source) == original,
            "hard-link AXML replacement lost source data");
    fs::remove(destination);
#ifndef _WIN32
    // Windows symlink creation requires privileges that the test runner may not have.
    fs::create_symlink(source, destination);
    rewritten = mradm::audio::replace_wav_axml(source.string(), destination.string(), "<new/>!");
    require(rewritten.has_value() && load(destination) == expected && load(source) == original,
            "symlink AXML replacement lost source data");
    fs::remove(destination);
#endif

    const auto missing_axml = wave("RIFF", {{"fmt ", format}, {"data", audio}});
    save(source, missing_axml);
    save(destination, expected);
    require(!mradm::audio::replace_wav_axml(source.string(), destination.string(), "<new/>!"), "missing AXML accepted");
    require(load(source) == missing_axml && load(destination) == expected,
            "failed AXML rewrite changed an existing file");
    require(!mradm::audio::replace_wav_axml(source.string(), source.string(), "<new/>!"),
            "missing AXML accepted in place");
    require(load(source) == missing_axml, "failed in-place AXML rewrite changed the source");

    const auto directory = root / "directory.wav";
    fs::create_directory(directory);
    save(directory / "keep", text_bytes("keep"));
    save(source, original);
    require(!mradm::audio::replace_wav_axml(source.string(), directory.string(), "<new/>!"),
            "directory destination accepted");
    require(load(directory / "keep") == text_bytes("keep") && load(source) == original,
            "failed AXML installation changed the directory or source");
    const bool leftovers = std::ranges::any_of(fs::directory_iterator(root), [](const fs::directory_entry& entry) {
        return entry.path().filename().string().find(".axml_tmp.") != std::string::npos;
    });
    require(!leftovers, "AXML rewrite left a temporary file behind");
}

} // namespace

int main() {
    const auto root = fs::temp_directory_path() / "mradm-wav-container-test";
    try {
        fs::remove_all(root);
        fs::create_directories(root);
        const auto all = fixtures();
        for (const auto& fixture : all) {
            if (fixture.audio_supported) {
                finalize_parity(root, fixture);
            } else {
                const auto path = root / "metadata-only.wav";
                save(path, fixture.bytes);
                const auto reader = mradm::audio::FloatWavReader::open(path.string());
                require(code_of(reader) == static_cast<int>(mradm::ErrorCode::unsupported),
                        "metadata-only encoding unexpectedly accepted for audio decoding");
            }
            metadata_parity(root, fixture);
            read_parity(root, fixture);
            replace_parity(root, fixture);
        }
        scan_parity(root, all);
        rust_only_checks(root, all);
        replace_alias_checks(root);
        fs::remove_all(root);
        std::cout << case_count() << " WAVE container cases match the frozen C++ implementation\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        fs::remove_all(root, ignored);
        return 1;
    }
}
