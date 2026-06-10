#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Our library (mradm namespace)
#include "adm/io.h"
#include "adm/scene.h"

// libadm / libbw64 — used here only to construct and inspect the fixture file.
#include <adm/adm.hpp>
#include <adm/utilities/id_assignment.hpp>
#include <adm/write.hpp>
#include <bw64/bw64.hpp>

namespace {

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

// One Objects-chain document with several non-default block parameters so we can
// confirm that fields the override does NOT touch survive the write-back.
// az=30 el=10 gain=0.8 diffuse=0.3 width=20.
std::pair<std::shared_ptr<adm::Document>, std::string> make_objects_doc() {
    auto doc = adm::Document::create();

    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"ExpCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{30.0F}, adm::Elevation{10.0F}}};
        block.set(adm::Gain{0.8F});
        block.set(adm::Diffuse{0.3F});
        block.set(adm::Width{20.0F});
        block.set(adm::JumpPosition{adm::JumpPositionFlag{false}, adm::InterpolationLength{std::chrono::seconds{1}}});
        cf->add(block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"ExpPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"ExpSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"ExpTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto object = adm::AudioObject::create(adm::AudioObjectName{"ExpObject"});
    object->set(adm::Gain{1.0F});
    object->addReference(uid);
    doc->add(object);

    auto content = adm::AudioContent::create(adm::AudioContentName{"ExpContent"});
    content->addReference(object);
    doc->add(content);

    auto programme = adm::AudioProgramme::create(adm::AudioProgrammeName{"ExpProgramme"});
    programme->addReference(content);
    doc->add(programme);

    adm::reassignIds(doc);
    return {doc, adm::formatId(uid->get<adm::AudioTrackUidId>())};
}

std::string serialize_doc(const std::shared_ptr<adm::Document>& doc) {
    std::ostringstream buf;
    adm::writeXml(buf, doc);
    return buf.str();
}

// 24-bit PCM ramp; values are chosen so a 24-bit round-trip is bit-exact.
std::vector<float> ramp_samples(uint64_t frames) {
    std::vector<float> samples(frames);
    for (uint64_t i = 0; i < frames; ++i) {
        samples[i] = -0.5F + (static_cast<float>(i % 100) / 100.0F);
    }
    return samples;
}

std::filesystem::path
write_fixture(const std::string& uid_str, const std::string& xml_str, const std::vector<float>& samples) {
    auto path = std::filesystem::temp_directory_path() / "mr_adm_export_src.wav";
    auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1, uid_str, "", "")});
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_str);
    auto writer = bw64::writeFile(path.string(), 1U, 48000U, 24U, chna, axml);
    auto mutable_samples = samples;
    writer->write(mutable_samples.data(), mutable_samples.size());
    return path;
}

uint32_t le_u32(const char* p) {
    return static_cast<uint32_t>(static_cast<unsigned char>(p[0])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8U) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16U) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24U);
}

// Read the raw bytes of the `data` chunk payload (no sample decode), so the test
// can assert the PCM was copied byte-for-byte (true bit-exact), not just within a
// quantization tolerance.
std::vector<char> read_data_chunk_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::array<char, 12> header{};
    in.read(header.data(), header.size());
    if (in.gcount() != static_cast<std::streamsize>(header.size())) {
        return {};
    }
    const bool is_rf64 = (header[0] == 'R' && header[1] == 'F' && header[2] == '6' && header[3] == '4') ||
                         (header[0] == 'B' && header[1] == 'W' && header[2] == '6' && header[3] == '4');
    in.seekg(0, std::ios::end);
    const auto file_size = static_cast<uint64_t>(in.tellg());

    bool have_ds64 = false;
    uint64_t ds64_data_size = 0;
    uint64_t pos = 12;
    while (pos + 8 <= file_size) {
        std::array<char, 8> ch{};
        in.seekg(static_cast<std::streamoff>(pos));
        in.read(ch.data(), ch.size());
        if (in.gcount() != static_cast<std::streamsize>(ch.size())) {
            break;
        }
        const uint32_t size32 = le_u32(ch.data() + 4);
        const uint64_t payload_off = pos + 8;
        uint64_t payload_size = size32;
        const bool is_ds64 = ch[0] == 'd' && ch[1] == 's' && ch[2] == '6' && ch[3] == '4';
        const bool is_data = ch[0] == 'd' && ch[1] == 'a' && ch[2] == 't' && ch[3] == 'a';
        if (is_ds64) {
            std::array<char, 16> ds{};
            in.read(ds.data(), ds.size());
            if (in.gcount() == static_cast<std::streamsize>(ds.size())) {
                have_ds64 = true;
                ds64_data_size = static_cast<uint64_t>(le_u32(ds.data() + 8)) |
                                 (static_cast<uint64_t>(le_u32(ds.data() + 12)) << 32U);
            }
        } else if (is_data && is_rf64 && size32 == 0xFFFFFFFFU) {
            payload_size = have_ds64 ? ds64_data_size : size32;
        }
        if (is_data) {
            std::vector<char> bytes(static_cast<std::size_t>(payload_size));
            in.seekg(static_cast<std::streamoff>(payload_off));
            in.read(bytes.data(), static_cast<std::streamsize>(payload_size));
            return bytes;
        }
        pos = payload_off + payload_size + (payload_size & 1ULL);
    }
    return {};
}

const mradm::SceneObjectBlock* first_block(const mradm::AdmScene& scene) {
    if (scene.objects.empty() || scene.objects[0].tracks.empty() || scene.objects[0].tracks[0].blocks.empty()) {
        return nullptr;
    }
    return scene.objects[0].tracks[0].blocks.data();
}

// Round-trip with no overrides: the re-imported scene matches the source and the
// data chunk PCM is copied byte-for-byte (bit-exact).
bool verify_export_roundtrip() {
    bool ok = true;
    auto [doc, uid_str] = make_objects_doc();
    constexpr uint64_t k_frames = 480;
    auto src = write_fixture(uid_str, serialize_doc(doc), ramp_samples(k_frames));
    FileGuard src_guard{src};

    auto original = mradm::io::import_scene(src.string());
    if (!original) {
        std::cerr << "FAIL: roundtrip source import: " << original.error().message << "\n";
        return false;
    }

    auto dst = std::filesystem::temp_directory_path() / "mr_adm_export_roundtrip.wav";
    FileGuard dst_guard{dst};
    auto written = mradm::io::write_scene(src.string(), *original, *original, dst.string());
    if (!written) {
        std::cerr << "FAIL: write_scene roundtrip: " << written.error().message << "\n";
        return false;
    }

    auto reimported = mradm::io::import_scene(dst.string());
    if (!reimported) {
        std::cerr << "FAIL: roundtrip output import: " << reimported.error().message << "\n";
        return false;
    }

    const auto* before = first_block(*original);
    const auto* after = first_block(*reimported);
    ok &= check(before != nullptr && after != nullptr, "roundtrip: both scenes have a block");
    if (before != nullptr && after != nullptr) {
        ok &= check(std::fabs(after->position.azimuth - before->position.azimuth) < 0.01F, "roundtrip: azimuth kept");
        ok &= check(std::fabs(after->position.elevation - before->position.elevation) < 0.01F,
                    "roundtrip: elevation kept");
        ok &= check(std::fabs(after->gain - before->gain) < 0.001F, "roundtrip: gain kept");
        ok &= check(std::fabs(after->diffuse - before->diffuse) < 0.001F, "roundtrip: diffuse kept");
        ok &= check(std::fabs(after->width - before->width) < 0.01F, "roundtrip: width kept");
    }

    // The chunk-level rewriter copies the data chunk byte-for-byte, so assert the
    // raw PCM payload is bit-identical (not merely within a quantization tolerance).
    const auto src_data = read_data_chunk_bytes(src.string());
    const auto dst_data = read_data_chunk_bytes(dst.string());
    ok &= check(!src_data.empty(), "roundtrip: source has a data chunk");
    ok &= check(src_data == dst_data, "roundtrip: data chunk byte-for-byte identical (bit-exact)");

    if (ok) {
        std::cout << "PASS: verify_export_roundtrip\n";
    }
    return ok;
}

// Gain override: write back a changed block + object gain; everything else stays.
bool verify_export_gain_override() {
    bool ok = true;
    auto [doc, uid_str] = make_objects_doc();
    auto src = write_fixture(uid_str, serialize_doc(doc), ramp_samples(96));
    FileGuard src_guard{src};

    auto original = mradm::io::import_scene(src.string());
    if (!original) {
        std::cerr << "FAIL: gain override source import: " << original.error().message << "\n";
        return false;
    }

    mradm::AdmScene effective = *original;
    effective.objects[0].gain *= 0.5F;
    effective.objects[0].tracks[0].blocks[0].gain *= 0.5F;

    auto dst = std::filesystem::temp_directory_path() / "mr_adm_export_gain.wav";
    FileGuard dst_guard{dst};
    auto written = mradm::io::write_scene(src.string(), *original, effective, dst.string());
    if (!written) {
        std::cerr << "FAIL: write_scene gain override: " << written.error().message << "\n";
        return false;
    }

    auto reimported = mradm::io::import_scene(dst.string());
    if (!reimported) {
        std::cerr << "FAIL: gain override output import: " << reimported.error().message << "\n";
        return false;
    }

    ok &= check(!reimported->objects.empty(), "gain override: object present");
    if (!reimported->objects.empty()) {
        ok &= check(std::fabs(reimported->objects[0].gain - 0.5F) < 0.001F, "gain override: object gain halved");
    }
    const auto* blk = first_block(*reimported);
    ok &= check(blk != nullptr, "gain override: block present");
    if (blk != nullptr) {
        ok &= check(std::fabs(blk->gain - 0.4F) < 0.001F, "gain override: block gain 0.8 -> 0.4");
        // Untouched fields stay put.
        ok &= check(std::fabs(blk->position.azimuth - 30.0F) < 0.01F, "gain override: azimuth unchanged");
        ok &= check(std::fabs(blk->diffuse - 0.3F) < 0.001F, "gain override: diffuse unchanged");
        ok &= check(std::fabs(blk->width - 20.0F) < 0.01F, "gain override: width unchanged");
    }

    if (ok) {
        std::cout << "PASS: verify_export_gain_override\n";
    }
    return ok;
}

// interpolation.max_ms changes the block interpolationLength; unlike position, it
// is representation-stable and should be written back.
bool verify_export_interpolation_override() {
    bool ok = true;
    auto [doc, uid_str] = make_objects_doc();
    auto src = write_fixture(uid_str, serialize_doc(doc), ramp_samples(96));
    FileGuard src_guard{src};

    auto original = mradm::io::import_scene(src.string());
    if (!original) {
        std::cerr << "FAIL: interpolation override source import: " << original.error().message << "\n";
        return false;
    }

    const auto* before = first_block(*original);
    ok &= check(before != nullptr && before->interp_length_samples == 48000U,
                "interpolation override: source has 1s interpolationLength");

    mradm::AdmScene effective = *original;
    effective.objects[0].tracks[0].blocks[0].interp_length_samples = 12000U;

    auto dst = std::filesystem::temp_directory_path() / "mr_adm_export_interpolation.wav";
    FileGuard dst_guard{dst};
    auto written = mradm::io::write_scene(src.string(), *original, effective, dst.string());
    if (!written) {
        std::cerr << "FAIL: write_scene interpolation override: " << written.error().message << "\n";
        return false;
    }

    auto reimported = mradm::io::import_scene(dst.string());
    if (!reimported) {
        std::cerr << "FAIL: interpolation override output import: " << reimported.error().message << "\n";
        return false;
    }

    const auto* blk = first_block(*reimported);
    ok &= check(blk != nullptr, "interpolation override: block present");
    if (blk != nullptr) {
        ok &= check(blk->interp_length_samples == 12000U, "interpolation override: length 1s -> 0.25s");
        ok &= check(std::fabs(blk->gain - 0.8F) < 0.001F, "interpolation override: gain unchanged");
        ok &= check(std::fabs(blk->position.azimuth - 30.0F) < 0.01F,
                    "interpolation override: azimuth unchanged");
    }

    ok &= check(read_data_chunk_bytes(src.string()) == read_data_chunk_bytes(dst.string()),
                "interpolation override: data chunk byte-for-byte identical (bit-exact)");

    if (ok) {
        std::cout << "PASS: verify_export_interpolation_override\n";
    }
    return ok;
}

// Position override is intentionally not written back yet; coordinate-representation
// changes need a separate design. The export should preserve the source position.
bool verify_export_position_override_deferred() {
    bool ok = true;
    auto [doc, uid_str] = make_objects_doc();
    auto src = write_fixture(uid_str, serialize_doc(doc), ramp_samples(96));
    FileGuard src_guard{src};

    auto original = mradm::io::import_scene(src.string());
    if (!original) {
        std::cerr << "FAIL: position override source import: " << original.error().message << "\n";
        return false;
    }

    mradm::AdmScene effective = *original;
    effective.objects[0].tracks[0].blocks[0].position.azimuth = -90.0F;
    effective.objects[0].tracks[0].blocks[0].position.elevation = 0.0F;

    auto dst = std::filesystem::temp_directory_path() / "mr_adm_export_position.wav";
    FileGuard dst_guard{dst};
    auto written = mradm::io::write_scene(src.string(), *original, effective, dst.string());
    if (!written) {
        std::cerr << "FAIL: write_scene deferred position override: " << written.error().message << "\n";
        return false;
    }

    auto reimported = mradm::io::import_scene(dst.string());
    if (!reimported) {
        std::cerr << "FAIL: deferred position override output import: " << reimported.error().message << "\n";
        return false;
    }

    const auto* blk = first_block(*reimported);
    ok &= check(blk != nullptr, "deferred position override: block present");
    if (blk != nullptr) {
        ok &= check(!blk->position.cartesian, "deferred position override: still polar");
        ok &= check(std::fabs(blk->position.azimuth - 30.0F) < 0.01F,
                    "deferred position override: azimuth unchanged");
        ok &= check(std::fabs(blk->position.elevation - 10.0F) < 0.01F,
                    "deferred position override: elevation unchanged");
        // Untouched fields stay put.
        ok &= check(std::fabs(blk->gain - 0.8F) < 0.001F, "deferred position override: gain unchanged");
        ok &= check(std::fabs(blk->diffuse - 0.3F) < 0.001F, "deferred position override: diffuse unchanged");
    }

    if (ok) {
        std::cout << "PASS: verify_export_position_override_deferred\n";
    }
    return ok;
}

// Surgically convert a libbw64-written RIFF fixture into an equivalent BW64 file:
// flip the tag to BW64, insert a ds64 chunk after WAVE, and set the data chunk's
// 32-bit size field to the 0xFFFFFFFF sentinel (real size lives in ds64.dataSize).
// PCM / chna / fmt / axml payloads are copied unchanged — just enough to exercise
// the exporter's BW64 path (tag preservation + ds64.bw64Size rewrite).
std::filesystem::path riff_fixture_to_bw64(const std::filesystem::path& riff_path) {
    std::ifstream in(riff_path, std::ios::binary);
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    std::vector<char> src(size);
    in.read(src.data(), static_cast<std::streamsize>(size));
    in.close();

    struct Chunk {
        std::array<char, 4> id{};
        std::size_t payload_off{0};
        uint64_t size{0};
    };
    std::vector<Chunk> chunks;
    uint64_t data_size = 0;
    std::size_t pos = 12;
    while (pos + 8 <= src.size()) {
        Chunk c;
        c.id = {src[pos], src[pos + 1], src[pos + 2], src[pos + 3]};
        c.size = le_u32(src.data() + pos + 4);
        c.payload_off = pos + 8;
        if (c.id[0] == 'd' && c.id[1] == 'a' && c.id[2] == 't' && c.id[3] == 'a') {
            data_size = c.size;
        }
        chunks.push_back(c);
        pos = c.payload_off + c.size + (c.size & 1ULL);
    }

    auto bw64_path = std::filesystem::temp_directory_path() / "mr_adm_export_src_bw64.wav";
    std::ofstream out(bw64_path, std::ios::binary | std::ios::trunc);
    const auto put_u32 = [&](uint32_t v) {
        const std::array<char, 4> b{static_cast<char>(v & 0xFFU),
                                    static_cast<char>((v >> 8U) & 0xFFU),
                                    static_cast<char>((v >> 16U) & 0xFFU),
                                    static_cast<char>((v >> 24U) & 0xFFU)};
        out.write(b.data(), b.size());
    };
    const auto put_u64 = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            const auto c = static_cast<char>(v & 0xFFULL);
            out.write(&c, 1);
            v >>= 8U;
        }
    };

    uint64_t body = 4 + 8 + 28; // "WAVE" + ds64 header + ds64 payload
    for (const auto& c : chunks) {
        body += 8 + c.size + (c.size & 1ULL);
    }

    out.write("BW64", 4);
    put_u32(0xFFFFFFFFU);
    out.write("WAVE", 4);
    out.write("ds64", 4);
    put_u32(28);
    put_u64(body);      // bw64Size = file size - 8
    put_u64(data_size); // dataSize
    put_u64(data_size); // sampleCount (unverified here)
    put_u32(0);         // tableLength
    for (const auto& c : chunks) {
        const bool is_data = c.id[0] == 'd' && c.id[1] == 'a' && c.id[2] == 't' && c.id[3] == 'a';
        out.write(c.id.data(), 4);
        put_u32(is_data ? 0xFFFFFFFFU : static_cast<uint32_t>(c.size));
        out.write(src.data() + c.payload_off, static_cast<std::streamsize>(c.size));
        if ((c.size & 1ULL) != 0U) {
            out.put('\0');
        }
    }
    return bw64_path;
}

// BW64 round-trip: the exporter must keep the BW64 container tag and rewrite
// ds64.bw64Size, with the data chunk still byte-for-byte identical.
bool verify_export_bw64_roundtrip() {
    bool ok = true;
    auto [doc, uid_str] = make_objects_doc();
    auto riff = write_fixture(uid_str, serialize_doc(doc), ramp_samples(480));
    FileGuard riff_guard{riff};
    auto bw64_src = riff_fixture_to_bw64(riff);
    FileGuard bw64_guard{bw64_src};

    auto original = mradm::io::import_scene(bw64_src.string());
    if (!original) {
        std::cerr << "FAIL: bw64 source import: " << original.error().message << "\n";
        return false;
    }

    auto dst = std::filesystem::temp_directory_path() / "mr_adm_export_bw64_out.wav";
    FileGuard dst_guard{dst};
    auto written = mradm::io::write_scene(bw64_src.string(), *original, *original, dst.string());
    if (!written) {
        std::cerr << "FAIL: write_scene bw64: " << written.error().message << "\n";
        return false;
    }

    std::ifstream out_in(dst.string(), std::ios::binary);
    std::array<char, 4> tag{};
    out_in.read(tag.data(), tag.size());
    ok &= check(tag[0] == 'B' && tag[1] == 'W' && tag[2] == '6' && tag[3] == '4',
                "bw64: output keeps the BW64 container tag");

    ok &= check(read_data_chunk_bytes(bw64_src.string()) == read_data_chunk_bytes(dst.string()),
                "bw64: data chunk byte-for-byte identical (bit-exact)");

    auto reimported = mradm::io::import_scene(dst.string());
    ok &= check(reimported.has_value(), "bw64: output re-imports as valid ADM");

    if (ok) {
        std::cout << "PASS: verify_export_bw64_roundtrip\n";
    }
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= verify_export_roundtrip();
    ok &= verify_export_gain_override();
    ok &= verify_export_interpolation_override();
    ok &= verify_export_position_override_deferred();
    ok &= verify_export_bw64_roundtrip();
    return ok ? 0 : 1;
}
