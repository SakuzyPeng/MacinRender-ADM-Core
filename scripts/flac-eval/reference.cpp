// Standalone selection probe. Neither this TU nor dr_flac enters a new production target.
#define DR_FLAC_IMPLEMENTATION
#include <algorithm>
#include <bit>
#include <cstdint>
#include <dr_flac.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <FLAC/stream_encoder.h>

namespace {
void put32(std::ostream& out, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        out.put(static_cast<char>((value >> shift) & 255));
    }
}

void encode(char** argv) {
    std::ifstream input(argv[2], std::ios::binary);
    if (!input) {
        throw std::runtime_error("open PCM failed");
    }
    std::vector<FLAC__int32> pcm;
    char bytes[4];
    while (input.read(bytes, 4)) {
        uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) {
            value |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << (i * 8);
        }
        pcm.push_back(std::bit_cast<int32_t>(value));
    }
    if (!input.eof() || input.gcount() != 0) {
        throw std::runtime_error("incomplete PCM input");
    }
    const auto bits = static_cast<unsigned>(std::stoul(argv[4]));
    const auto channels = static_cast<unsigned>(std::stoul(argv[5]));
    const auto rate = static_cast<unsigned>(std::stoul(argv[6]));
    if (channels == 0 || pcm.size() % channels != 0) {
        throw std::runtime_error("invalid channels or frame length");
    }
    std::unique_ptr<FLAC__StreamEncoder, decltype(&FLAC__stream_encoder_delete)> encoder(FLAC__stream_encoder_new(),
                                                                                         FLAC__stream_encoder_delete);
    auto* enc = encoder.get();
    if (!enc || !FLAC__stream_encoder_set_streamable_subset(enc, false) ||
        !FLAC__stream_encoder_set_channels(enc, channels) || !FLAC__stream_encoder_set_bits_per_sample(enc, bits) ||
        !FLAC__stream_encoder_set_sample_rate(enc, rate) || !FLAC__stream_encoder_set_compression_level(enc, 5) ||
        !FLAC__stream_encoder_set_blocksize(enc, 1024) ||
        !FLAC__stream_encoder_set_total_samples_estimate(enc, pcm.size() / channels) ||
        FLAC__stream_encoder_init_file(enc, argv[3], nullptr, nullptr) != FLAC__STREAM_ENCODER_INIT_STATUS_OK ||
        !FLAC__stream_encoder_process_interleaved(enc, pcm.data(), static_cast<unsigned>(pcm.size() / channels)) ||
        !FLAC__stream_encoder_finish(enc)) {
        throw std::runtime_error("libFLAC encode failed");
    }
}

void decode(int argc, char** argv) {
    const auto closer = [](drflac* value) { drflac_close(value); };
    std::unique_ptr<drflac, decltype(closer)> ints(drflac_open_file(argv[2], nullptr), closer);
    std::unique_ptr<drflac, decltype(closer)> floats(drflac_open_file(argv[2], nullptr), closer);
    if (!ints || !floats) {
        std::cout << "status=open_error\n";
        return;
    }
    std::cout << "channels=" << ints->channels << "\nrate=" << ints->sampleRate << "\nbits=" << ints->bitsPerSample
              << "\nframes_hint=" << ints->totalPCMFrameCount << '\n';
    if (argc > 4) {
        const auto target = std::stoull(argv[4]);
        if (!drflac_seek_to_pcm_frame(ints.get(), target) || !drflac_seek_to_pcm_frame(floats.get(), target)) {
            std::cout << "status=seek_error\n";
            return;
        }
    }
    std::ofstream out(argv[3], std::ios::binary | std::ios::trunc);
    const std::vector<uint64_t> blocks{1, 7, 127, 511, 1024};
    std::vector<drflac_int32> integers(1024 * ints->channels);
    std::vector<float> values(integers.size());
    uint64_t total = 0;
    size_t block = 0;
    for (;;) {
        const auto requested = blocks[block++ % blocks.size()];
        const auto got = drflac_read_pcm_frames_s32(ints.get(), requested, integers.data());
        if (drflac_read_pcm_frames_f32(floats.get(), requested, values.data()) != got) {
            throw std::runtime_error("integer/float length mismatch");
        }
        if (got == 0) {
            break;
        }
        for (size_t i = 0; i < got * ints->channels; ++i) {
            put32(out, std::bit_cast<uint32_t>(integers[i]));
            put32(out, std::bit_cast<uint32_t>(values[i]));
        }
        total += got;
    }
    out.flush();
    if (!out) {
        throw std::runtime_error("PCM output failed");
    }
    std::cout << "frames_read=" << total << "\nstatus=read_zero\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 7 && std::string(argv[1]) == "encode") {
            encode(argv);
        } else if ((argc == 4 || argc == 5) && std::string(argv[1]) == "drflac") {
            decode(argc, argv);
        } else {
            throw std::runtime_error("encode pcm flac bits channels rate | drflac input output [seek]");
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
