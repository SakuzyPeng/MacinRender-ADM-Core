#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <adm/adm.hpp>
#include <adm/utilities/id_assignment.hpp>
#include <adm/write.hpp>
#include <bw64/bw64.hpp>

#include "adm/audio_io.h"
#include "adm/render.h"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

// getpid() 在 POSIX 来自 <unistd.h>，Windows 用 <process.h> 的 _getpid()。
[[nodiscard]] int current_process_id() {
#ifdef _WIN32
    return _getpid();
#else
    return ::getpid();
#endif
}

class FileGuard {
  public:
    explicit FileGuard(std::filesystem::path path) : path_(std::move(path)) {}
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    FileGuard(FileGuard&&) = delete;
    FileGuard& operator=(FileGuard&&) = delete;
    ~FileGuard() { std::filesystem::remove(path_); }

  private:
    std::filesystem::path path_;
};

bool check(bool condition, const char* msg) {
    if (!condition) {
        std::cerr << "FAIL: " << msg << "\n";
    }
    return condition;
}

std::filesystem::path temp_path(std::string_view stem, std::string_view ext) {
    static std::atomic<int> s_seq{0};
    const auto name = std::string(stem) + "_" + std::to_string(current_process_id()) + "_" +
                      std::to_string(s_seq.fetch_add(1)) + std::string(ext);
    return std::filesystem::temp_directory_path() / name;
}

std::pair<std::shared_ptr<adm::Document>, std::string> make_render_doc() {
    auto doc = adm::Document::create();
    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"TrimCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
        block.set(adm::Gain{1.0F});
        cf->add(block);
    }
    doc->add(cf);
    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"TrimPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);
    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"TrimSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);
    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"TrimTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);
    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);
    auto obj = adm::AudioObject::create(adm::AudioObjectName{"TrimObject"});
    obj->addReference(uid);
    doc->add(obj);
    auto content = adm::AudioContent::create(adm::AudioContentName{"TrimContent"});
    content->addReference(obj);
    doc->add(content);
    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"TrimProgramme"});
    prog->addReference(content);
    doc->add(prog);
    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

std::pair<std::shared_ptr<adm::Document>, std::vector<std::string>> make_multi_object_doc(uint32_t channels) {
    auto doc = adm::Document::create();
    std::vector<std::string> uid_strings;
    uid_strings.reserve(channels);
    std::vector<std::shared_ptr<adm::AudioTrackUid>> uids;
    uids.reserve(channels);

    auto content = adm::AudioContent::create(adm::AudioContentName{"LargeContent"});
    doc->add(content);
    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"LargeProgramme"});
    prog->addReference(content);
    doc->add(prog);

    for (uint32_t ch = 0; ch < channels; ++ch) {
        const std::string suffix = std::to_string(ch + 1U);
        auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{std::string{"LargeCF"} + suffix},
                                                  adm::TypeDefinition::OBJECTS);
        {
            const float az = -60.0F + (static_cast<float>(ch) * 24.0F);
            adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{az}, adm::Elevation{0.0F}}};
            block.set(adm::Gain{0.5F});
            cf->add(block);
        }
        doc->add(cf);

        auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{std::string{"LargePF"} + suffix},
                                               adm::TypeDefinition::OBJECTS);
        pf->addReference(cf);
        doc->add(pf);

        auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{std::string{"LargeSF"} + suffix},
                                                 adm::FormatDefinition::PCM);
        sf->setReference(cf);
        doc->add(sf);

        auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{std::string{"LargeTF"} + suffix},
                                                adm::FormatDefinition::PCM);
        tf->setReference(sf);
        sf->addReference(tf);
        doc->add(tf);

        auto uid = adm::AudioTrackUid::create();
        uid->setReference(tf);
        uid->setReference(pf);
        doc->add(uid);
        uids.push_back(uid);

        auto obj = adm::AudioObject::create(adm::AudioObjectName{std::string{"LargeObject"} + suffix});
        obj->addReference(uid);
        doc->add(obj);
        content->addReference(obj);
    }

    adm::reassignIds(doc);
    std::ranges::transform(uids, std::back_inserter(uid_strings), [](const auto& uid) {
        return adm::formatId(uid->template get<adm::AudioTrackUidId>());
    });
    return {doc, uid_strings};
}

constexpr uint32_t k_sr = 48000U;
constexpr uint32_t k_frames = k_sr * 2U; // 2 seconds

[[nodiscard]] uint32_t read_le32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8U) | (static_cast<uint32_t>(p[2]) << 16U) |
           (static_cast<uint32_t>(p[3]) << 24U);
}

[[nodiscard]] uint64_t read_le64(const unsigned char* p) {
    return static_cast<uint64_t>(read_le32(p)) | (static_cast<uint64_t>(read_le32(p + 4U)) << 32U);
}

void write_le32(std::ostream& out, uint32_t value) {
    const std::array<unsigned char, 4> bytes{static_cast<unsigned char>(value),
                                             static_cast<unsigned char>(value >> 8U),
                                             static_cast<unsigned char>(value >> 16U),
                                             static_cast<unsigned char>(value >> 24U)};
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void write_le64(std::ostream& out, uint64_t value) {
    const std::array<unsigned char, 8> bytes{static_cast<unsigned char>(value),
                                             static_cast<unsigned char>(value >> 8U),
                                             static_cast<unsigned char>(value >> 16U),
                                             static_cast<unsigned char>(value >> 24U),
                                             static_cast<unsigned char>(value >> 32U),
                                             static_cast<unsigned char>(value >> 40U),
                                             static_cast<unsigned char>(value >> 48U),
                                             static_cast<unsigned char>(value >> 56U)};
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

struct WavChunk {
    std::array<char, 4> id{};
    std::size_t payload_offset{0};
    uint64_t payload_size{0};
};

[[nodiscard]] bool chunk_id_is(const WavChunk& chunk, std::string_view id) {
    return id.size() == 4U && std::string_view{chunk.id.data(), chunk.id.size()} == id;
}

[[nodiscard]] std::optional<std::vector<WavChunk>> parse_wave_chunks(const std::vector<unsigned char>& bytes) {
    if (bytes.size() < 12U || std::string_view{reinterpret_cast<const char*>(bytes.data() + 8U), 4U} != "WAVE") {
        return std::nullopt;
    }
    const std::string_view container{reinterpret_cast<const char*>(bytes.data()), 4U};
    const bool uses_ds64 = container == "RF64" || container == "BW64";
    uint64_t ds64_data_size = 0;
    bool have_ds64 = false;

    std::vector<WavChunk> chunks;
    std::size_t pos = 12U;
    while (pos + 8U <= bytes.size()) {
        WavChunk chunk;
        chunk.id = {static_cast<char>(bytes[pos]),
                    static_cast<char>(bytes[pos + 1U]),
                    static_cast<char>(bytes[pos + 2U]),
                    static_cast<char>(bytes[pos + 3U])};
        chunk.payload_offset = pos + 8U;
        const uint32_t size32 = read_le32(bytes.data() + pos + 4U);
        chunk.payload_size = size32;

        if (chunk_id_is(chunk, "ds64")) {
            if (chunk.payload_size < 28U || chunk.payload_offset + 28U > bytes.size()) {
                return std::nullopt;
            }
            ds64_data_size = read_le64(bytes.data() + chunk.payload_offset + 8U);
            have_ds64 = true;
        } else if (uses_ds64 && chunk_id_is(chunk, "data") && size32 == 0xFFFFFFFFU) {
            if (!have_ds64) {
                return std::nullopt;
            }
            chunk.payload_size = ds64_data_size;
        }

        if (chunk.payload_offset + chunk.payload_size > bytes.size()) {
            return std::nullopt;
        }
        chunks.push_back(chunk);
        pos = chunk.payload_offset + static_cast<std::size_t>(chunk.payload_size) +
              static_cast<std::size_t>(chunk.payload_size & 1ULL);
    }
    return chunks;
}

struct LargeBw64Fixture {
    std::filesystem::path path;
    uint64_t frames{0};
    uint32_t channels{0};
};

struct SparseBw64Layout {
    uint64_t frames{0};
    uint64_t data_bytes{0};
};

std::optional<std::filesystem::path> write_large_seed_fixture(uint32_t channels, uint32_t bits_per_sample) {
    constexpr uint64_t k_seed_frames = 32U;

    const auto [doc, uid_strings] = make_multi_object_doc(channels);
    if (!check(uid_strings.size() == channels, "large fixture UID count")) {
        return std::nullopt;
    }

    std::ostringstream xml_buf;
    adm::writeXml(xml_buf, doc);
    std::vector<bw64::AudioId> audio_ids;
    audio_ids.reserve(channels);
    for (uint32_t ch = 0; ch < channels; ++ch) {
        audio_ids.emplace_back(static_cast<uint16_t>(ch + 1U), uid_strings[ch], "", "");
    }
    auto chna = std::make_shared<bw64::ChnaChunk>(audio_ids);
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());

    auto path = temp_path("mr_trim_sparse_large_input", ".wav");
    auto writer = bw64::writeFile(path.string(),
                                  static_cast<uint16_t>(channels),
                                  static_cast<uint16_t>(k_sr),
                                  static_cast<uint16_t>(bits_per_sample),
                                  chna,
                                  axml);
    std::vector<float> samples(k_seed_frames * channels, 0.0F);
    writer->write(samples.data(), k_seed_frames);
    return path;
}

[[nodiscard]] std::optional<std::vector<unsigned char>> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)), {});
}

bool copy_seed_chunks_before_sparse_data(std::ofstream& out,
                                         const std::vector<unsigned char>& src,
                                         const std::vector<WavChunk>& chunks) {
    for (const auto& chunk : chunks) {
        if (chunk_id_is(chunk, "data") || chunk_id_is(chunk, "ds64")) {
            continue;
        }
        if (chunk.payload_size > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        out.write(chunk.id.data(), static_cast<std::streamsize>(chunk.id.size()));
        write_le32(out, static_cast<uint32_t>(chunk.payload_size));
        out.write(reinterpret_cast<const char*>(src.data() + chunk.payload_offset),
                  static_cast<std::streamsize>(chunk.payload_size));
        if ((chunk.payload_size & 1ULL) != 0U) {
            out.put('\0');
        }
    }
    return static_cast<bool>(out);
}

bool rewrite_seed_as_sparse_bw64(const std::filesystem::path& path, const SparseBw64Layout& layout) {
    auto src_opt = read_file_bytes(path);
    if (!src_opt) {
        check(false, "read seed BW64 fixture");
        std::filesystem::remove(path);
        return false;
    }
    std::vector<unsigned char> src = std::move(src_opt.value());

    auto chunks_opt = parse_wave_chunks(src);
    if (!chunks_opt) {
        check(false, "parse seed BW64 fixture chunks");
        std::filesystem::remove(path);
        return false;
    }
    std::vector<WavChunk> chunks = std::move(chunks_opt.value());
    const auto data_it = std::ranges::find_if(chunks, [](const WavChunk& chunk) { return chunk_id_is(chunk, "data"); });
    if (!check(data_it != chunks.end(), "seed BW64 fixture has data chunk")) {
        std::filesystem::remove(path);
        return false;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write("BW64", 4);
    write_le32(out, 0xFFFFFFFFU);
    out.write("WAVE", 4);
    out.write("ds64", 4);
    write_le32(out, 28U);
    const std::streamoff ds64_payload_offset = out.tellp();
    write_le64(out, 0U);
    write_le64(out, layout.data_bytes);
    write_le64(out, layout.frames);
    write_le32(out, 0U);

    if (!copy_seed_chunks_before_sparse_data(out, src, chunks)) {
        std::filesystem::remove(path);
        return false;
    }

    out.write("data", 4);
    write_le32(out, 0xFFFFFFFFU);
    const uint64_t data_payload_offset = static_cast<uint64_t>(out.tellp());
    out.write(reinterpret_cast<const char*>(src.data() + data_it->payload_offset),
              static_cast<std::streamsize>(data_it->payload_size));
    out.close();
    if (!check(static_cast<bool>(out), "write sparse BW64 fixture header")) {
        std::filesystem::remove(path);
        return false;
    }

    const uint64_t sparse_size = data_payload_offset + layout.data_bytes;
    std::filesystem::resize_file(path, sparse_size);

    std::fstream patch(path, std::ios::binary | std::ios::in | std::ios::out);
    patch.seekp(ds64_payload_offset);
    write_le64(patch, sparse_size - 8U);
    if (!check(static_cast<bool>(patch), "patch sparse BW64 ds64 size")) {
        std::filesystem::remove(path);
        return false;
    }
    return true;
}

std::optional<LargeBw64Fixture> write_sparse_large_bw64_fixture() {
    constexpr uint32_t k_channels = 6U;
    constexpr uint32_t k_bits_per_sample = 24U;
    constexpr uint64_t k_bytes_per_frame = uint64_t{k_channels} * (k_bits_per_sample / 8U);
    constexpr uint64_t k_big_frames = static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) + (uint64_t{2} * k_sr);
    constexpr uint64_t k_big_data_bytes = k_big_frames * k_bytes_per_frame;

    auto path = write_large_seed_fixture(k_channels, k_bits_per_sample);
    if (!path || !rewrite_seed_as_sparse_bw64(*path, {k_big_frames, k_big_data_bytes})) {
        return std::nullopt;
    }
    return LargeBw64Fixture{*path, k_big_frames, k_channels};
}

std::filesystem::path write_render_fixture() {
    const auto [doc, uid_str] = make_render_doc();
    auto path = temp_path("mr_trim_engine_input", ".wav");

    std::ostringstream xml_buf;
    adm::writeXml(xml_buf, doc);
    auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid_str, "", "")});
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());

    auto writer = bw64::writeFile(path.string(), 1U, k_sr, 24U, chna, axml);
    // Silent first second, active second second. The render pipeline is causal
    // (decorrelator FIR + direct delay), so the active half cannot leak backwards:
    // the first half stays exactly silent, letting a window-aware meter tell the
    // halves apart (silent half has no peak, active half does).
    std::vector<float> samples(k_frames, 0.0F);
    for (uint64_t f = k_frames / 2U; f < k_frames; ++f) {
        samples[f] = 0.5F;
    }
    writer->write(samples.data(), k_frames);
    return path;
}

// A diffuse, moving object so rendering exercises stateful paths the windowed
// pre-roll must warm up (direct compensation delay / decorrelator overlap) and
// block-edge smoothing must keep aligned with the full-render block grid.
std::pair<std::shared_ptr<adm::Document>, std::string> make_diffuse_doc() {
    auto doc = adm::Document::create();
    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"DiffCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block0{adm::SphericalPosition{adm::Azimuth{-30.0F}, adm::Elevation{0.0F}}};
        block0.set(adm::Rtime{adm::Time{std::chrono::milliseconds{0}}});
        block0.set(adm::Duration{adm::Time{std::chrono::milliseconds{520}}});
        block0.set(adm::JumpPosition{adm::JumpPositionFlag{true}});
        block0.set(adm::Gain{1.0F});
        block0.set(adm::Diffuse{0.5F}); // split energy across direct + decorrelated diffuse bus
        cf->add(block0);

        adm::AudioBlockFormatObjects block1{adm::SphericalPosition{adm::Azimuth{30.0F}, adm::Elevation{0.0F}}};
        block1.set(adm::Rtime{adm::Time{std::chrono::milliseconds{520}}});
        block1.set(adm::JumpPosition{adm::JumpPositionFlag{false}});
        block1.set(adm::Gain{1.0F});
        block1.set(adm::Diffuse{0.5F});
        cf->add(block1);
    }
    doc->add(cf);
    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"DiffPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);
    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"DiffSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);
    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"DiffTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);
    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);
    auto obj = adm::AudioObject::create(adm::AudioObjectName{"DiffObject"});
    obj->addReference(uid);
    doc->add(obj);
    auto content = adm::AudioContent::create(adm::AudioContentName{"DiffContent"});
    content->addReference(obj);
    doc->add(content);
    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"DiffProgramme"});
    prog->addReference(content);
    doc->add(prog);
    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

// 2 s of a per-sample-varying signal (so any pre-roll/delay error is visible) carried
// by a diffuse object.
std::filesystem::path write_varying_fixture() {
    const auto [doc, uid_str] = make_diffuse_doc();
    auto path = temp_path("mr_trim_vary_input", ".wav");
    std::ostringstream xml_buf;
    adm::writeXml(xml_buf, doc);
    auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid_str, "", "")});
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());
    auto writer = bw64::writeFile(path.string(), 1U, k_sr, 24U, chna, axml);
    std::vector<float> samples(k_frames);
    for (uint64_t f = 0; f < k_frames; ++f) {
        samples[f] = 0.5F * std::sin(static_cast<float>(f) * 0.013F);
    }
    writer->write(samples.data(), k_frames);
    return path;
}

// Render the 2-second fixture to a WAV with the given trim. Defaults to the ear
// backend at 5.1; the renderer/layout are overridable for backend-specific checks.
mradm::RenderResult render_with_trim(const std::filesystem::path& in_path,
                                     const std::filesystem::path& out_path,
                                     double start_sec,
                                     std::optional<double> end_sec,
                                     mradm::RendererSelection renderer = mradm::RendererSelection::ear,
                                     const std::string& layout = "0+5+0") {
    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.renderer = renderer;
    req.options.output_layout = layout;
    req.options.peak_limit = false;
    req.options.measure_loudness = false;
    req.options.render_start_sec = start_sec;
    req.options.render_end_sec = end_sec;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    return service.render(req, progress, logs);
}

bool wav_frame_count(const std::filesystem::path& path, uint64_t& out_frames) {
    auto reader_res = mradm::audio::FloatWavReader::open(path.string());
    if (!reader_res) {
        return false;
    }
    out_frames = reader_res->frame_count();
    return true;
}

// --end clips the tail: a 2 s input rendered with --end 1.0 yields ~1 s.
bool verify_end_trim() {
    const auto in_path = write_render_fixture();
    const FileGuard in_guard(in_path);
    const auto out_path = temp_path("mr_trim_end_out", ".wav");
    const FileGuard out_guard(out_path);

    const auto res = render_with_trim(in_path, out_path, 0.0, 1.0);
    if (!check(res.success(), "render with --end 1.0 succeeds")) {
        return false;
    }
    uint64_t frames = 0;
    if (!check(wav_frame_count(out_path, frames), "open --end output")) {
        return false;
    }
    return check(frames == k_sr, "--end 1.0 produces 1 second of output");
}

// --start and --end together select a 1 s window from the middle.
bool verify_start_end_window() {
    const auto in_path = write_render_fixture();
    const FileGuard in_guard(in_path);
    const auto out_path = temp_path("mr_trim_win_out", ".wav");
    const FileGuard out_guard(out_path);

    const auto res = render_with_trim(in_path, out_path, 0.5, 1.5);
    if (!check(res.success(), "render with --start 0.5 --end 1.5 succeeds")) {
        return false;
    }
    uint64_t frames = 0;
    if (!check(wav_frame_count(out_path, frames), "open window output")) {
        return false;
    }
    return check(frames == k_sr, "[0.5s, 1.5s) window produces 1 second of output");
}

// No trim options: output keeps the full 2 s duration.
bool verify_no_trim_full_duration() {
    const auto in_path = write_render_fixture();
    const FileGuard in_guard(in_path);
    const auto out_path = temp_path("mr_trim_full_out", ".wav");
    const FileGuard out_guard(out_path);

    const auto res = render_with_trim(in_path, out_path, 0.0, std::nullopt);
    if (!check(res.success(), "render without trim succeeds")) {
        return false;
    }
    uint64_t frames = 0;
    if (!check(wav_frame_count(out_path, frames), "open full output")) {
        return false;
    }
    return check(frames == k_frames, "no trim keeps full 2 second duration");
}

// --start beyond the input duration is rejected before rendering writes output.
bool verify_start_beyond_duration_rejected() {
    const auto in_path = write_render_fixture();
    const FileGuard in_guard(in_path);
    const auto out_path = temp_path("mr_trim_oob_out", ".wav");
    const FileGuard out_guard(out_path);

    const auto res = render_with_trim(in_path, out_path, 3.0, std::nullopt);
    if (!check(!res.success(), "render with --start past duration fails")) {
        return false;
    }
    if (!check(res.error.code == mradm::ErrorCode::invalid_argument, "out-of-range start returns invalid_argument")) {
        return false;
    }
    return check(!std::filesystem::exists(out_path), "no output written for out-of-range start");
}

// --end <= --start is rejected.
bool verify_end_not_after_start_rejected() {
    const auto in_path = write_render_fixture();
    const FileGuard in_guard(in_path);
    const auto out_path = temp_path("mr_trim_bad_out", ".wav");
    const FileGuard out_guard(out_path);

    const auto res = render_with_trim(in_path, out_path, 1.0, 1.0);
    if (!check(!res.success(), "render with --end == --start fails")) {
        return false;
    }
    return check(res.error.code == mradm::ErrorCode::invalid_argument, "--end <= --start returns invalid_argument");
}

// A window that passes the seconds-level --end > --start check but collapses to
// zero frames once rounded must be rejected before rendering, leaving no output.
bool verify_subframe_window_rejected() {
    const auto in_path = write_render_fixture();
    const FileGuard in_guard(in_path);
    const auto out_path = temp_path("mr_trim_subframe_out", ".wav");
    const FileGuard out_guard(out_path);

    // start at frame 48000; end only 0.4 of a frame later still rounds to 48000.
    const double start_sec = 1.0;
    const double end_sec = 1.0 + (0.4 / static_cast<double>(k_sr));
    const auto res = render_with_trim(in_path, out_path, start_sec, end_sec);
    if (!check(!res.success(), "render with sub-frame window fails")) {
        return false;
    }
    if (!check(res.error.code == mradm::ErrorCode::invalid_argument, "sub-frame window returns invalid_argument")) {
        return false;
    }
    return check(!std::filesystem::exists(out_path), "no output written for sub-frame window");
}

// The backend meters only the trimmed window, so loudness/True-Peak describe the
// kept segment: the silent first half reports no peak, the active second half
// does. If metering ignored the window, the silent half would still report the
// full render's peak.
bool verify_metrics_follow_trim_window() {
    const auto in_path = write_render_fixture();
    const FileGuard in_guard(in_path);

    const auto out_silent = temp_path("mr_trim_metric_silent", ".wav");
    const FileGuard guard_silent(out_silent);
    const auto res_silent = render_with_trim(in_path, out_silent, 0.0, 1.0);
    if (!check(res_silent.success(), "render silent-half segment succeeds")) {
        return false;
    }
    if (!check(res_silent.metrics.has_value() && !res_silent.metrics->measured_peak_dbtp.has_value(),
               "silent first-half segment reports no true-peak (meter followed the window)")) {
        return false;
    }

    const auto out_active = temp_path("mr_trim_metric_active", ".wav");
    const FileGuard guard_active(out_active);
    const auto res_active = render_with_trim(in_path, out_active, 1.0, std::nullopt);
    if (!check(res_active.success(), "render active-half segment succeeds")) {
        return false;
    }
    return check(res_active.metrics.has_value() && res_active.metrics->measured_peak_dbtp.has_value(),
                 "active second-half segment reports a true-peak");
}

// HOA-specific check: the HOA backend measures loudness/True-Peak by decoding the
// ambisonics output to a 7.1.4 reference domain (AllRAD) with LFE separated. This
// confirms the meter window is honoured through that decode path too: silent first
// half reports no peak, active second half does.
bool verify_hoa_metrics_follow_trim_window() {
    const auto in_path = write_render_fixture();
    const FileGuard in_guard(in_path);

    const auto out_silent = temp_path("mr_trim_hoa_silent", ".wav");
    const FileGuard guard_silent(out_silent);
    const auto res_silent = render_with_trim(in_path, out_silent, 0.0, 1.0, mradm::RendererSelection::hoa, "hoa3");
    if (!check(res_silent.success(), "HOA render silent-half segment succeeds")) {
        return false;
    }
    if (!check(res_silent.metrics.has_value() && !res_silent.metrics->measured_peak_dbtp.has_value(),
               "HOA silent first-half reports no true-peak (window honoured through AllRAD decode)")) {
        return false;
    }

    const auto out_active = temp_path("mr_trim_hoa_active", ".wav");
    const FileGuard guard_active(out_active);
    const auto res_active =
        render_with_trim(in_path, out_active, 1.0, std::nullopt, mradm::RendererSelection::hoa, "hoa3");
    if (!check(res_active.success(), "HOA render active-half segment succeeds")) {
        return false;
    }
    return check(res_active.metrics.has_value() && res_active.metrics->measured_peak_dbtp.has_value(),
                 "HOA active second-half reports a true-peak");
}

// Phase 1 core guarantee: on-demand window rendering (seek + pre-roll) is
// sample-identical to a full render then sliced. The window starts well past one
// k_block_size block so the seek + pre-roll path actually engages; the diffuse +
// varying-signal fixture means a missing/short pre-roll would corrupt the window head.
bool window_bit_exact(const std::filesystem::path& in_path,
                      mradm::RendererSelection renderer,
                      const std::string& layout,
                      const char* label,
                      float abs_tolerance = 0.0F) {
    const auto full_path = temp_path("mr_trim_be_full", ".wav");
    const FileGuard full_guard(full_path);
    const auto win_path = temp_path("mr_trim_be_win", ".wav");
    const FileGuard win_guard(win_path);

    // [24000, 40000) frames. start 24000 spans many minimum render blocks
    // (k_block_size >= 1024), forcing a real reader seek and (for stateful backends)
    // a pre-roll block.
    constexpr uint64_t k_start = 24000U;
    constexpr uint64_t k_count = 16000U;
    const double start_sec = static_cast<double>(k_start) / k_sr;
    const double end_sec = static_cast<double>(k_start + k_count) / k_sr;

    if (!check(render_with_trim(in_path, full_path, 0.0, std::nullopt, renderer, layout).success(),
               "full render succeeds")) {
        return false;
    }
    if (!check(render_with_trim(in_path, win_path, start_sec, end_sec, renderer, layout).success(),
               "windowed render succeeds")) {
        return false;
    }

    auto full = mradm::audio::FloatWavReader::open(full_path.string());
    auto win = mradm::audio::FloatWavReader::open(win_path.string());
    if (!check(static_cast<bool>(full) && static_cast<bool>(win), "open both render outputs")) {
        return false;
    }
    const uint32_t ch = full->channels();
    if (!check(win->channels() == ch, "channel counts match") ||
        !check(win->frame_count() == k_count, "windowed output is exactly the window length")) {
        return false;
    }

    std::vector<float> full_buf(full->frame_count() * ch);
    full->read(full_buf.data(), full->frame_count());
    std::vector<float> win_buf(k_count * ch);
    win->read(win_buf.data(), k_count);

    bool exact = true;
    uint64_t first_bad = 0;
    float first_expected = 0.0F;
    float first_actual = 0.0F;
    float max_abs_diff = 0.0F;
    for (uint64_t i = 0; i < k_count * ch; ++i) {
        const float actual = win_buf[i];
        const float expected = full_buf[(k_start * ch) + i];
        const float diff = std::abs(actual - expected);
        max_abs_diff = std::max(max_abs_diff, diff);
        if (diff > abs_tolerance && exact) {
            exact = false;
            first_bad = i;
            first_expected = expected;
            first_actual = actual;
        }
    }
    if (!exact) {
        std::cerr << "window mismatch detail: label=" << label << " sample_index=" << first_bad
                  << " frame=" << (first_bad / ch) << " channel=" << (first_bad % ch) << " expected=" << first_expected
                  << " actual=" << first_actual << " max_abs_diff=" << max_abs_diff << " tolerance=" << abs_tolerance
                  << "\n";
    }
    return check(exact, label);
}

// EAR (5.1), SAF/VBAP (5.1), HOA (hoa3), and binaural windowed renders must each
// match a full render then sliced. EAR/VBAP/HOA are bit-exact; binaural allows a
// narrow float tolerance for platform math differences in the HRTF overlap path.
bool verify_window_bit_exact_vs_full() {
    const auto in_path = write_varying_fixture();
    const FileGuard in_guard(in_path);
    bool ok = window_bit_exact(in_path, mradm::RendererSelection::ear, "0+5+0", "ear: window == full sliced");
    ok = window_bit_exact(in_path, mradm::RendererSelection::saf, "0+5+0", "vbap: window == full sliced") && ok;
    ok = window_bit_exact(in_path, mradm::RendererSelection::hoa, "hoa3", "hoa: window == full sliced") && ok;
    ok = window_bit_exact(
             in_path, mradm::RendererSelection::binaural, "0+2+0", "binaural: window ~= full sliced", 1.0e-6F) &&
         ok;
    return ok;
}

bool verify_sparse_large_bw64_input_multichannel_window() {
    const auto fixture = write_sparse_large_bw64_fixture();
    if (!fixture) {
        return false;
    }
    const FileGuard in_guard(fixture->path);
    const auto out_path = temp_path("mr_trim_sparse_large_out", ".wav");
    const FileGuard out_guard(out_path);

    constexpr uint64_t k_window_frames = 1024U;
    const uint64_t start_frame = fixture->frames - (2U * k_window_frames);
    if (!check(start_frame > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()),
               "sparse large BW64 render seeks beyond libbw64 int32 frame offset")) {
        return false;
    }
    const double start_sec = static_cast<double>(start_frame) / static_cast<double>(k_sr);
    const double end_sec = static_cast<double>(start_frame + k_window_frames) / static_cast<double>(k_sr);

    const auto res =
        render_with_trim(fixture->path, out_path, start_sec, end_sec, mradm::RendererSelection::saf, "0+5+0");
    if (!res.success()) {
        std::cerr << "FAIL: sparse large BW64 window render: " << res.error.message << "\n";
        return false;
    }

    auto reader = mradm::audio::FloatWavReader::open(out_path.string());
    if (!check(static_cast<bool>(reader), "open sparse large BW64 render output")) {
        return false;
    }
    bool ok = check(reader->channels() == fixture->channels, "sparse large BW64 render writes 5.1 output");
    ok = check(reader->frame_count() == k_window_frames, "sparse large BW64 render writes requested window") && ok;
#ifdef __APPLE__
    const auto verify_apple_layout = [&](std::string_view layout, uint32_t expected_channels) {
        const auto apple_out_path = temp_path("mr_trim_sparse_large_apple_out", ".wav");
        const FileGuard apple_out_guard(apple_out_path);
        const auto apple_res = render_with_trim(
            fixture->path, apple_out_path, start_sec, end_sec, mradm::RendererSelection::apple, std::string{layout});
        if (!apple_res.success()) {
            std::cerr << "FAIL: sparse large BW64 Apple window render (" << layout << "): " << apple_res.error.message
                      << "\n";
            return false;
        }
        auto apple_reader = mradm::audio::FloatWavReader::open(apple_out_path.string());
        if (!check(static_cast<bool>(apple_reader), "open sparse large BW64 Apple render output")) {
            return false;
        }
        bool layout_ok = check(apple_reader->channels() == expected_channels,
                               "sparse large BW64 Apple render writes expected channel count");
        layout_ok = check(apple_reader->frame_count() == k_window_frames,
                          "sparse large BW64 Apple render writes requested window") &&
                    layout_ok;
        return layout_ok;
    };
    ok = verify_apple_layout("4+7+0", 12U) && ok;
    ok = verify_apple_layout("9+10+3", 24U) && ok;
#endif
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= verify_end_trim();
    ok &= verify_start_end_window();
    ok &= verify_no_trim_full_duration();
    ok &= verify_start_beyond_duration_rejected();
    ok &= verify_end_not_after_start_rejected();
    ok &= verify_subframe_window_rejected();
    ok &= verify_metrics_follow_trim_window();
    ok &= verify_hoa_metrics_follow_trim_window();
    ok &= verify_window_bit_exact_vs_full();
    ok &= verify_sparse_large_bw64_input_multichannel_window();
    if (!ok) {
        return EXIT_FAILURE;
    }
    std::cout << "render trim fixture tests passed (10/10)\n";
    return EXIT_SUCCESS;
}
