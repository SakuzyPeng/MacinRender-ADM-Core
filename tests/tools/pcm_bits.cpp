// PCM bit-pattern extraction and comparison for cross-platform numerical baselines.
//
// Why this exists: the revised determinism contract (docs/adr/0008, roadmap §6.1) requires
// comparing the final float32 PCM by *bit pattern*, not by container hash and not by float
// tolerance. Container hashes are unusable because every output writes the current UTC time
// (render_service.cpp populates date_utc -> bext OriginationDate/Time, FLAC DATE=, Opus
// DATE_RECORDED, CAF date), so two identical renders differ as files. FLAC additionally
// quantises float32 to 24-bit. Absolute-difference helpers such as maximum_difference() and
// window_bit_exact() cannot express the contract either: a zero absolute difference does not
// separate +0.0 from -0.0, and a difference comparison without an explicit finite check can
// let NaN through.
//
// Modes:
//   extract <audio-in> <bits-out>   Decode PCM, reject non-finite samples, write the canonical
//                                   little-endian bit image. Prints one metadata line.
//   compare <a> <b>                 Bit-compare two inputs (either .pcmbits images or audio
//                                   files). On mismatch, report the first differing frame and
//                                   channel with both bit patterns, plus max absolute error and
//                                   ULP distribution to help localise the failing stage.
//
// The canonical image is what CI compares across runners: the three platforms each `extract`,
// upload the images, and a downstream job compares them. Because the image is byte-canonical,
// a plain byte comparison of two images is exactly the contract's bit comparison.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "adm/audio_io.h"

namespace {

// Canonical image header: magic, version, channels, sample_rate, frame_count. All little-endian.
// Metadata is part of the compared bytes on purpose — a channel-count or rate change is a
// difference in the rendered result, not an incidental detail.
constexpr std::array<char, 4> k_magic{'M', 'R', 'P', 'B'};
constexpr uint32_t k_version = 1;
constexpr std::size_t k_header_bytes = 4 + 4 + 4 + 4 + 8;

struct PcmImage {
    uint32_t channels{0};
    uint32_t sample_rate{0};
    uint64_t frames{0};
    std::vector<uint32_t> bits; // frames * channels, interleaved, host order
};

[[nodiscard]] bool valid_shape(const std::string& path, const PcmImage& img) {
    if (img.channels == 0U || img.sample_rate == 0U ||
        img.frames > std::numeric_limits<std::size_t>::max() / img.channels) {
        std::cerr << "error: invalid or overflowing PCM shape in " << path << "\n";
        return false;
    }
    return true;
}

void put_u32_le(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFU));
    out.push_back(static_cast<uint8_t>((v >> 8U) & 0xFFU));
    out.push_back(static_cast<uint8_t>((v >> 16U) & 0xFFU));
    out.push_back(static_cast<uint8_t>((v >> 24U) & 0xFFU));
}

void put_u64_le(std::vector<uint8_t>& out, uint64_t v) {
    put_u32_le(out, static_cast<uint32_t>(v & 0xFFFFFFFFU));
    put_u32_le(out, static_cast<uint32_t>((v >> 32U) & 0xFFFFFFFFU));
}

[[nodiscard]] uint32_t get_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8U) | (static_cast<uint32_t>(p[2]) << 16U) |
           (static_cast<uint32_t>(p[3]) << 24U);
}

[[nodiscard]] uint64_t get_u64_le(const uint8_t* p) {
    return static_cast<uint64_t>(get_u32_le(p)) | (static_cast<uint64_t>(get_u32_le(p + 4)) << 32U);
}

[[nodiscard]] bool read_file_bytes(const std::string& path, std::vector<uint8_t>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        std::cerr << "error: cannot open " << path << "\n";
        return false;
    }
    constexpr std::size_t k_read_chunk = std::size_t{64} * 1024;
    std::array<uint8_t, k_read_chunk> buf{};
    std::size_t n = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
    }
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    if (!ok) {
        std::cerr << "error: read failed for " << path << "\n";
    }
    return ok;
}

// Decode an audio file to interleaved float32 and capture the bit patterns. Non-finite samples
// are rejected here rather than compared: NaN never equals itself, so letting one through would
// turn a hard failure into an unnoticed one.
[[nodiscard]] bool load_audio(const std::string& path, PcmImage& img) {
    auto reader_res = mradm::audio::ReaderHandle::open(path);
    if (!reader_res) {
        std::cerr << "error: cannot open audio " << path << ": " << reader_res.error().message << "\n";
        return false;
    }
    auto& reader = *reader_res;
    img.channels = reader.channels();
    img.sample_rate = reader.sample_rate();
    img.frames = reader.frame_count();
    if (!valid_shape(path, img)) {
        return false;
    }

    constexpr uint64_t k_chunk_frames = 8192;
    std::vector<float> chunk(static_cast<std::size_t>(k_chunk_frames) * img.channels);
    img.bits.reserve(static_cast<std::size_t>(img.frames) * img.channels);

    uint64_t done = 0;
    while (done < img.frames) {
        const uint64_t want = std::min<uint64_t>(k_chunk_frames, img.frames - done);
        const uint64_t got = reader.read(chunk.data(), want);
        if (got == 0U) {
            break;
        }
        const std::size_t count = static_cast<std::size_t>(got) * img.channels;
        for (std::size_t i = 0; i < count; ++i) {
            const float s = chunk[i];
            if (!std::isfinite(s)) {
                const uint64_t frame = done + (i / img.channels);
                const std::size_t ch = i % img.channels;
                std::cerr << "error: non-finite sample in " << path << " at frame " << frame << " channel " << ch
                          << " (bits=0x" << std::hex << std::bit_cast<uint32_t>(s) << std::dec << ")\n";
                return false;
            }
            img.bits.push_back(std::bit_cast<uint32_t>(s));
        }
        done += got;
    }
    if (done != img.frames) {
        std::cerr << "error: " << path << " declared " << img.frames << " frames but decoded " << done << "\n";
        return false;
    }
    return true;
}

[[nodiscard]] bool load_image_file(const std::string& path, PcmImage& img) {
    std::vector<uint8_t> raw;
    if (!read_file_bytes(path, raw)) {
        return false;
    }
    if (raw.size() < k_header_bytes || std::memcmp(raw.data(), k_magic.data(), k_magic.size()) != 0) {
        std::cerr << "error: " << path << " is not a .pcmbits image\n";
        return false;
    }
    const uint32_t version = get_u32_le(raw.data() + 4);
    if (version != k_version) {
        std::cerr << "error: " << path << " has image version " << version << ", expected " << k_version << "\n";
        return false;
    }
    img.channels = get_u32_le(raw.data() + 8);
    img.sample_rate = get_u32_le(raw.data() + 12);
    img.frames = get_u64_le(raw.data() + 16);
    if (!valid_shape(path, img)) {
        return false;
    }

    const std::size_t payload = raw.size() - k_header_bytes;
    if ((payload % 4U) != 0U) {
        std::cerr << "error: " << path << " payload is not a whole number of samples\n";
        return false;
    }
    const std::size_t samples = payload / 4U;
    const std::size_t expect = static_cast<std::size_t>(img.frames) * img.channels;
    if (samples != expect) {
        std::cerr << "error: " << path << " header declares " << expect << " samples but payload holds " << samples
                  << "\n";
        return false;
    }
    img.bits.resize(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        img.bits[i] = get_u32_le(raw.data() + k_header_bytes + (i * 4U));
        if (!std::isfinite(std::bit_cast<float>(img.bits[i]))) {
            std::cerr << "error: non-finite sample in " << path << " at frame " << (i / img.channels) << " channel "
                      << (i % img.channels) << "\n";
            return false;
        }
    }
    return true;
}

// Accept either form so the same binary is usable locally on raw renders and in CI on images.
[[nodiscard]] bool load_any(const std::string& path, PcmImage& img) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        std::cerr << "error: cannot open " << path << "\n";
        return false;
    }
    std::array<char, 4> head{};
    const std::size_t n = std::fread(head.data(), 1, head.size(), f);
    std::fclose(f);
    const bool is_image = n == head.size() && std::memcmp(head.data(), k_magic.data(), k_magic.size()) == 0;
    return is_image ? load_image_file(path, img) : load_audio(path, img);
}

[[nodiscard]] bool write_image(const std::string& path, const PcmImage& img) {
    std::vector<uint8_t> out;
    out.reserve(k_header_bytes + (img.bits.size() * 4U));
    out.insert(out.end(), k_magic.begin(), k_magic.end());
    put_u32_le(out, k_version);
    put_u32_le(out, img.channels);
    put_u32_le(out, img.sample_rate);
    put_u64_le(out, img.frames);
    for (const uint32_t b : img.bits) {
        put_u32_le(out, b);
    }

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        std::cerr << "error: cannot write " << path << "\n";
        return false;
    }
    const std::size_t wrote = std::fwrite(out.data(), 1, out.size(), f);
    const int close_status = std::fclose(f);
    const bool ok = wrote == out.size() && close_status == 0;
    if (!ok) {
        std::cerr << "error: short write for " << path << "\n";
    }
    return ok;
}

// Map a float onto a monotonically ordered integer so that ULP distance is a plain subtraction
// across the sign boundary. Negative bit patterns run in reverse order; complement them,
// then put positive patterns above them. -0 and +0 are adjacent (distance 1). Used only for
// diagnostics — the pass/fail decision stays a bit comparison, per the contract.
[[nodiscard]] int64_t ordered_key(uint32_t bits) {
    constexpr uint32_t k_sign = 0x80000000U;
    return static_cast<int64_t>((bits & k_sign) != 0U ? ~bits : (bits | k_sign));
}

void print_meta(const std::string& label, const PcmImage& img) {
    std::cout << label << " channels=" << img.channels << " sample_rate=" << img.sample_rate << " frames=" << img.frames
              << " samples=" << img.bits.size() << "\n";
}

[[nodiscard]] int run_extract(const std::string& in_path, const std::string& out_path) {
    PcmImage img;
    if (!load_audio(in_path, img)) {
        return 2;
    }
    if (!write_image(out_path, img)) {
        return 2;
    }
    print_meta("extracted", img);
    return 0;
}

[[nodiscard]] int run_compare(const std::string& a_path, const std::string& b_path) {
    PcmImage a;
    PcmImage b;
    if (!load_any(a_path, a) || !load_any(b_path, b)) {
        return 2;
    }

    if (a.channels != b.channels || a.sample_rate != b.sample_rate || a.frames != b.frames) {
        std::cout << "DIFFER: shape mismatch\n";
        print_meta("  a:", a);
        print_meta("  b:", b);
        return 1;
    }

    int64_t max_ulp = 0;
    double max_abs = 0.0;
    std::size_t diff_count = 0;
    std::size_t first_index = 0;
    bool have_first = false;

    for (std::size_t i = 0; i < a.bits.size(); ++i) {
        if (a.bits[i] == b.bits[i]) {
            continue;
        }
        ++diff_count;
        if (!have_first) {
            first_index = i;
            have_first = true;
        }
        const auto fa = std::bit_cast<float>(a.bits[i]);
        const auto fb = std::bit_cast<float>(b.bits[i]);
        max_abs = std::max(max_abs, std::abs(static_cast<double>(fa) - static_cast<double>(fb)));
        const int64_t ulp = std::abs(ordered_key(a.bits[i]) - ordered_key(b.bits[i]));
        max_ulp = std::max(max_ulp, ulp);
    }

    if (diff_count == 0) {
        std::cout << "IDENTICAL samples=" << a.bits.size() << " frames=" << a.frames << " channels=" << a.channels
                  << "\n";
        return 0;
    }

    const uint64_t frame = static_cast<uint64_t>(first_index) / a.channels;
    const std::size_t channel = first_index % a.channels;
    const auto fa = std::bit_cast<float>(a.bits[first_index]);
    const auto fb = std::bit_cast<float>(b.bits[first_index]);
    std::cout << "DIFFER: " << diff_count << " of " << a.bits.size() << " samples\n"
              << "  first: sample_index=" << first_index << " frame=" << frame << " channel=" << channel << "\n"
              << "    a=" << fa << " (bits=0x" << std::hex << a.bits[first_index] << std::dec << ")\n"
              << "    b=" << fb << " (bits=0x" << std::hex << b.bits[first_index] << std::dec << ")\n"
              << "  max_abs_error=" << max_abs << " max_ulp=" << max_ulp << "\n"
              << "  note: error metrics locate the failing stage; they do not relax the bit-equality gate.\n";
    return 1;
}

void print_usage() {
    std::cerr << "usage:\n"
              << "  mr_adm_pcm_bits extract <audio-in> <bits-out>\n"
              << "  mr_adm_pcm_bits validate <image>     # validate a canonical .pcmbits image\n"
              << "  mr_adm_pcm_bits compare <a> <b>        # .pcmbits images or audio files\n"
              << "exit: 0 identical / 1 differ / 2 usage or IO error\n";
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv, argv + argc);
    if (args.size() < 2) {
        print_usage();
        return 2;
    }
    const std::string_view mode = args[1];
    if (mode == "validate" && args.size() == 3) {
        PcmImage img;
        return load_image_file(std::string{args[2]}, img) ? 0 : 2;
    }
    if (mode == "extract" && args.size() == 4) {
        return run_extract(std::string{args[2]}, std::string{args[3]});
    }
    if (mode == "compare" && args.size() == 4) {
        return run_compare(std::string{args[2]}, std::string{args[3]});
    }
    print_usage();
    return 2;
}
