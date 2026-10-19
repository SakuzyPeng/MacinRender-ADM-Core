// Standalone CPU emulation of the candidate pack/install kernels; apply candidate.patch first.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
#define __device__
#define __global__
#define FFT_N 4096
#define PARTS 28
#define BUCKETS 10
#define BUCKET_STRIDE 768
#define TARGETS 32
struct Index {
    unsigned x = 0, y = 0;
} blockIdx, threadIdx, blockDim{128, 1};
#include "../../../../scripts/research/room_gpu/session_kernels.cu"
static_assert(sizeof(Update) == 276 && sizeof(Header) == 20);
void require(bool value) {
    if (!value)
        throw std::runtime_error("Packed layout mismatch");
}
int main() {
    constexpr unsigned supports[] = {1, 256, 257, 777, 2048, 4096, 7071};
    const float sentinel = std::numeric_limits<float>::quiet_NaN();
    for (unsigned count : {0u, 1u, 3u, 17u, 256u}) {
        Header h{32, count * 3, count, 0, 0};
        std::vector<Update> updates(count);
        for (unsigned u = 0; u < count; ++u) {
            auto& v = updates[u];
            v.group = u % 8;
            v.slot = u / 8;
            v.support = supports[u % 7];
            v.parts = (v.support + 255) / 256;
            v.first_row = h.fft_rows;
            h.fft_rows += v.parts * 2;
            std::fill(std::begin(v.rows), std::end(v.rows), 0xffffffffu);
            for (unsigned b : {0u, 2u, 4u})
                v.rows[b] = u * 3 + b / 2;
        }
        std::vector<float> buckets(std::max(1u, h.ranges) * 2 * FFT_N);
        for (size_t i = 0; i < buckets.size(); ++i)
            buckets[i] = float(int(i % 1007) - 503) * .001f;
        std::vector<float> packed(std::max(128u, h.fft_rows) * 512 + 512, sentinel);
        for (blockIdx.y = 0; blockIdx.y < (count + 1) * 2; ++blockIdx.y)
            for (blockIdx.x = 0; blockIdx.x < PARTS * 4; ++blockIdx.x)
                for (threadIdx.x = 0; threadIdx.x < 128; ++threadIdx.x)
                    session_pack(&h, updates.data(), buckets.data(), packed.data());
        // Independent time-domain oracle: assemble each complete FIR first,
        // then check its 256-sample sections and each FFT window's zero half.
        for (unsigned u = 0; u < count; ++u) {
            const auto& v = updates[u];
            for (unsigned ear = 0; ear < 2; ++ear) {
                std::vector<float> fir(v.parts * 256, 0.f);
                for (unsigned b = 0; b < BUCKETS; ++b) {
                    unsigned r = v.rows[b];
                    if (r == 0xffffffffu)
                        continue;
                    for (unsigned t = b * BUCKET_STRIDE; t < v.support && t < b * BUCKET_STRIDE + FFT_N; ++t)
                        fir[t] += buckets[(r * 2 + ear) * FFT_N + t - b * BUCKET_STRIDE] * (1.f / FFT_N);
                }
                for (unsigned part = 0; part < v.parts; ++part)
                    for (unsigned t = 0; t < 512; ++t) {
                        float expected = t < 256 ? fir[part * 256 + t] : 0.f;
                        float actual = packed[(v.first_row + ear * v.parts + part) * 512 + t];
                        require(std::bit_cast<uint32_t>(expected) == std::bit_cast<uint32_t>(actual));
                    }
            }
        }
        for (size_t i = h.fft_rows * 512; i < packed.size(); ++i)
            require(std::isnan(packed[i]));
        std::vector<Complex> fresh(std::max(128u, h.fft_rows) * 257, {sentinel, sentinel});
        for (unsigned row = 0; row < h.fft_rows; ++row)
            for (unsigned bin = 0; bin < 257; ++bin)
                fresh[row * 257 + bin] = {float(row + 1), float(bin)};
        std::vector<Complex> filters(8 * TARGETS * 2 * PARTS * 257 + 257, {sentinel, sentinel});
        for (blockIdx.y = 0; blockIdx.y <= count; ++blockIdx.y)
            for (blockIdx.x = 0; blockIdx.x < (2 * PARTS * 257 + 127) / 128; ++blockIdx.x)
                for (threadIdx.x = 0; threadIdx.x < 128; ++threadIdx.x)
                    session_install(&h, updates.data(), fresh.data(), filters.data());
        for (unsigned u = 0; u < count; ++u) {
            const auto& v = updates[u];
            for (unsigned ear = 0; ear < 2; ++ear)
                for (unsigned part = 0; part < PARTS; ++part)
                    for (unsigned bin = 0; bin < 257; ++bin) {
                        auto actual = filters[(((v.group * TARGETS + v.slot) * 2 + ear) * PARTS + part) * 257 + bin];
                        if (part < v.parts) {
                            require(actual.r == float(v.first_row + ear * v.parts + part + 1));
                            require(actual.i == float(bin));
                        } else
                            require(std::isnan(actual.r) && std::isnan(actual.i));
                    }
        }
        for (size_t i = filters.size() - 257; i < filters.size(); ++i)
            require(std::isnan(filters[i].r));
        std::cout << "updates=" << count << " active_fft_rows=" << h.fft_rows << " passed\n";
    }
}
