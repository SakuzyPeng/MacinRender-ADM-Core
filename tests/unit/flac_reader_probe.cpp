// FLAC reader island probe for flac_reader_test.py: decodes a FLAC file with the production
// reader (strict Rust validation, then dr_flac PCM) into the canonical .pcmbits image of
// mr_adm_pcm_bits. Exit 0 on success, 2 with "error: ..." on stderr otherwise; the output file is
// written only on success. The general PCM tool (WAV / CAF) is the Rust mr_adm_pcm_bits.
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "adm/audio_io.h"

#include "../support/rust_services_test.h"

namespace {
void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (8U * i)) & 0xFFU));
    }
}

int extract(const std::string& input, const std::string& output) {
    auto reader = mradm::audio::FloatFlacReader::open(input);
    if (!reader) {
        std::cerr << "error: cannot open audio " << input << ": " << reader.error().message << "\n";
        return 2;
    }
    const uint32_t channels = reader->channels();
    const uint64_t frames = reader->frame_count();
    std::vector<uint8_t> image{'M', 'R', 'P', 'B'};
    put_u32(image, 1);
    put_u32(image, channels);
    put_u32(image, reader->sample_rate());
    put_u32(image, static_cast<uint32_t>(frames & 0xFFFFFFFFU));
    put_u32(image, static_cast<uint32_t>(frames >> 32U));
    constexpr uint64_t k_chunk = 8192;
    std::vector<float> chunk(static_cast<std::size_t>(k_chunk) * channels);
    uint64_t done = 0;
    while (done < frames) {
        const auto want = std::min(k_chunk, frames - done);
        const auto got = reader->read(chunk.data(), want);
        if (!got) {
            std::cerr << "error: cannot decode " << input << ": " << got.error().message << "\n";
            return 2;
        }
        if (*got == 0) {
            break;
        }
        for (std::size_t i = 0; i < static_cast<std::size_t>(*got) * channels; ++i) {
            if (!std::isfinite(chunk[i])) {
                std::cerr << "error: non-finite sample in " << input << "\n";
                return 2;
            }
            put_u32(image, std::bit_cast<uint32_t>(chunk[i]));
        }
        done += *got;
    }
    if (done != frames) {
        std::cerr << "error: " << input << " declared " << frames << " frames but decoded " << done << "\n";
        return 2;
    }
    std::FILE* f = std::fopen(output.c_str(), "wb");
    if (f == nullptr) {
        std::cerr << "error: cannot write " << output << "\n";
        return 2;
    }
    const bool ok = std::fwrite(image.data(), 1, image.size(), f) == image.size();
    if (std::fclose(f) != 0 || !ok) {
        std::cerr << "error: short write for " << output << "\n";
        return 2;
    }
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    mradm_test::bind_rust_services();
    if (argc != 4 || std::string{argv[1]} != "extract") {
        std::cerr << "usage: mr_adm_flac_reader_probe extract <in.flac> <bits-out>\n";
        return 2;
    }
    return extract(argv[2], argv[3]);
}
