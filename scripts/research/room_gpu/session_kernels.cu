// Full Session batch executor. Target clocks and finite-fade masks come from
// the original sample schedule; physical slots are not reused within a block.
struct Complex {
    float r, i;
};
struct Term {
    unsigned row, fraction, shift;
    float alpha, weight;
};
struct Job {
    unsigned material, fraction, shift, counts;
    float alpha, gain;
    Term terms[12];
};
struct Range {
    unsigned first, count;
};
struct Update {
    unsigned group, slot, parts, support, first_row;
    unsigned rows[64];
};
struct Task {
    unsigned long long start, end, block_start;
    unsigned slot, branch, parts, begin, frames, first;
};
struct Info {
    unsigned active, reset, heads[2];
};
struct Header {
    unsigned voices, ranges, updates, tasks, fft_rows;
};
#ifndef ROOM_GPU_FFT_LIBRARY
__device__ Complex plus(Complex a, Complex b) {
    return {a.r + b.r, a.i + b.i};
}
__device__ Complex product(Complex a, Complex b) {
    return {a.r * b.r - a.i * b.i, a.r * b.i + a.i * b.r};
}
__device__ Complex times(Complex a, float b) {
    return {a.r * b, a.i * b};
}
__device__ Complex interpolation(const Complex* data, unsigned row, unsigned bin, float alpha) {
    Complex a = data[row * (FFT_N / 2 + 1) + bin], b = data[(row + 1) * (FFT_N / 2 + 1) + bin];
    return {a.r + (b.r - a.r) * alpha, a.i + (b.i - a.i) * alpha};
}
extern "C" __global__ void session_response(const Header* h,
                                            const Range* ranges,
                                            const Job* jobs,
                                            const Complex* bank,
                                            const Complex* fractions,
                                            const Complex* propagation,
                                            const Complex* materials,
                                            const Complex* phases,
                                            Complex* out) {
#ifdef ROOM_GPU_FREQUENCY_MAJOR
    unsigned bin = blockIdx.y * blockDim.x + threadIdx.x, row = blockIdx.x;
#else
    unsigned bin = blockIdx.x * blockDim.x + threadIdx.x, row = blockIdx.y;
#endif
    if (bin >= FFT_N / 2 + 1)
        return;
    Complex sum = {0, 0};
    if (row / 2 < h->ranges) {
        Range range = ranges[row / 2];
        unsigned ear = row % 2;
        for (unsigned p = 0; p < range.count; ++p) {
            const Job& job = jobs[range.first + p];
            Complex value = {0, 0};
            for (unsigned t = 0; t < ((job.counts >> (8 * ear)) & 255); ++t) {
                Term term = job.terms[ear * 6 + t];
                Complex v = product(bank[term.row * (FFT_N / 2 + 1) + bin],
                                    interpolation(fractions, term.fraction, bin, term.alpha));
                v = product(v, phases[(term.shift * bin) & (FFT_N - 1)]);
                value = plus(value, times(v, term.weight));
            }
            if (bin == 0 || bin == FFT_N / 2)
                value.i = 0;
            Complex transfer = product(interpolation(propagation, job.fraction, bin, job.alpha),
                                       phases[(job.shift * bin) & (FFT_N - 1)]);
            transfer = times(product(transfer, materials[job.material * (FFT_N / 2 + 1) + bin]), job.gain);
            sum = plus(sum, product(value, transfer));
        }
    }
    out[row * (FFT_N / 2 + 1) + bin] = sum;
}
extern "C" __global__ void session_pack(const Header* h, const Update* updates, const float* buckets, float* windows) {
    unsigned i = blockIdx.x * blockDim.x + threadIdx.x, row = blockIdx.y;
    if (i >= PARTS * 512)
        return;
    unsigned target = row / 2, ear = row % 2, part = i / 512, t = part * 256 + i % 512;
    if (target >= h->updates)
        return;
    const Update& update = updates[target];
    if (part >= update.parts)
        return;
    float sum = 0;
    if (i % 512 < 256 && t < update.support) {
        for (unsigned b = 0; b < BUCKETS; ++b) {
            unsigned r = update.rows[b];
            int at = int(t) - int(b * BUCKET_STRIDE);
            if (r != 0xffffffffu && at >= 0 && at < FFT_N)
                sum += buckets[(r * 2 + ear) * FFT_N + at] * (1.f / FFT_N);
        }
    }
    windows[(update.first_row + ear * update.parts + part) * 512 + i % 512] = sum;
}
extern "C" __global__ void
session_install(const Header* h, const Update* updates, const Complex* fresh, Complex* filters) {
    unsigned i = blockIdx.x * blockDim.x + threadIdx.x, target = blockIdx.y;
    if (target >= h->updates || i >= 2 * PARTS * 257)
        return;
    Update u = updates[target];
    unsigned ear = i / (PARTS * 257), part = (i / 257) % PARTS, bin = i % 257;
    if (part >= u.parts)
        return;
    filters[((u.group * TARGETS + u.slot) * 2 * PARTS * 257) + i] =
        fresh[(u.first_row + ear * u.parts + part) * 257 + bin];
}
extern "C" __global__ void session_input(const Header* h, const Info* info, const Complex* input, Complex* history) {
    unsigned bin = blockIdx.x * blockDim.x + threadIdx.x, group = blockIdx.y;
    if (group / 2 >= h->voices || bin >= 257)
        return;
    Info v = info[group / 2];
    if (v.reset) {
        for (unsigned p = 0; p < PARTS; ++p)
            history[(group * PARTS + p) * 257 + bin] = {0, 0};
    }
    if (v.active)
        history[(group * PARTS + v.heads[group % 2]) * 257 + bin] = input[group * 257 + bin];
}
extern "C" __global__ void session_convolve(const Header* h,
                                            const Info* info,
                                            const Task* tasks,
                                            const Complex* history,
                                            const Complex* filters,
                                            Complex* out) {
    unsigned bin = blockIdx.x * blockDim.x + threadIdx.x, row = blockIdx.y;
    if (bin >= 257)
        return;
    Complex sum = {0, 0};
    if (row / 2 < h->tasks) {
        Task t = tasks[row / 2];
        unsigned group = t.branch, head = info[group / 2].heads[group % 2];
        for (unsigned p = 0; p < t.parts; ++p) {
            unsigned at = (head + PARTS - p) % PARTS;
            Complex x = history[(group * PARTS + at) * 257 + bin];
            Complex f = filters[(((group * TARGETS + t.slot) * 2 + row % 2) * PARTS + p) * 257 + bin];
            sum = plus(sum, product(x, f));
        }
    }
    out[row * 257 + bin] = sum;
}
extern "C" __global__ void
session_mix(const Header* h, const Range* groups, const Task* tasks, const float* cache, float* out, unsigned fade) {
    unsigned i = blockIdx.x * blockDim.x + threadIdx.x, group = blockIdx.y;
    if (i >= 512 || group / 2 >= h->voices)
        return;
    unsigned ear = i / 256, index = i % 256;
    float value = 0;
    Range range = groups[group];
    for (unsigned j = 0; j < range.count; ++j) {
        unsigned id = range.first + j;
        Task t = tasks[id];
        if (index < t.begin || index >= t.begin + t.frames)
            continue;
        unsigned long long sample = t.block_start + index, right = sample < t.end ? sample : t.end;
        unsigned long long duration;
        if (t.first)
            duration = sample - right >= fade ? 0 : fade - (sample - right);
        else {
            unsigned long long left = sample >= fade ? sample - fade : 0;
            left = left > t.start ? left : t.start;
            duration = right > left ? right - left : 0;
        }
        float weight = float(duration) / float(fade);
        value += (cache[(id * 2 + ear) * 512 + 256 + index] * (1.f / 512)) * weight;
    }
    out[group * 512 + i] = value;
}
extern "C" __global__ void
late_input(const unsigned* parameters, const Complex* input, Complex* history, unsigned parts) {
    unsigned bin = blockIdx.x * blockDim.x + threadIdx.x;
    if (bin >= 2049)
        return;
    if (parameters[1])
        for (unsigned p = 0; p < parts; ++p)
            history[p * 2049 + bin] = {0, 0};
    history[parameters[0] * 2049 + bin] = input[bin];
}
extern "C" __global__ void late_convolve(
    const unsigned* parameters, const Complex* history, const Complex* filters, Complex* out, unsigned parts) {
    unsigned bin = blockIdx.x * blockDim.x + threadIdx.x, direction = blockIdx.y;
    if (bin >= 2049)
        return;
    Complex sum = {0, 0};
    for (unsigned p = 0; p < parts; ++p) {
        unsigned at = (parameters[0] + parts - p) % parts;
        sum = plus(sum, product(history[at * 2049 + bin], filters[(direction * parts + p) * 2049 + bin]));
    }
    out[direction * 2049 + bin] = sum;
}
extern "C" __global__ void late_extract(const float* time, float* out) {
    unsigned i = blockIdx.x * blockDim.x + threadIdx.x, direction = blockIdx.y;
    out[direction * 2048 + i] = time[direction * 4096 + 2048 + i] * (1.f / 4096);
}


#endif // ROOM_GPU_FFT_LIBRARY

#if defined(ROOM_GPU_FUSE_TARGET) || defined(ROOM_GPU_FUSE_CONVOLVE)
#include <cufftdx.hpp>
namespace room_fft {
using namespace cufftdx;
using Common = decltype(Block() + Size<512>() + Precision<float>() + SM<ROOM_FFT_SM>() + ElementsPerThread<8>() +
                        FFTsPerBlock<4>());
using Forward = decltype(Common() + Type<fft_type::r2c>() + Direction<fft_direction::forward>() +
                         RealFFTOptions<complex_layout::natural, real_mode::folded>());
using Inverse = decltype(Common() + Type<fft_type::c2r>() + Direction<fft_direction::inverse>() +
                         RealFFTOptions<complex_layout::natural, real_mode::folded>());
static_assert(!Forward::requires_workspace && !Inverse::requires_workspace);
static_assert(Forward::input_length == 256 && Forward::output_length == 257);
static_assert(Inverse::input_length == 257 && Inverse::output_length == 256);
static_assert(sizeof(Forward::input_type) == 8 && sizeof(Inverse::output_type) == 8);
static_assert(Forward::implicit_type_batching == 1 && Inverse::implicit_type_batching == 1);
} // namespace room_fft
#endif

#ifdef ROOM_GPU_FUSE_TARGET
struct TargetConfig {
    unsigned buckets, bucket_stride, parts;
};
extern "C" __device__ __constant__ unsigned session_target_fft_contract[] = {1, FFT_N, TARGETS, sizeof(TargetConfig)};
extern "C" __device__ __constant__ unsigned session_target_fft_traits[] = {room_fft::Forward::block_dim.x,
                                                                           room_fft::Forward::block_dim.y,
                                                                           room_fft::Forward::shared_memory_size,
                                                                           room_fft::Forward::ffts_per_block};

template <unsigned BucketCount>
__device__ float target_time_sample(
    const Update& update, unsigned ear, unsigned part, unsigned sample, const float* buckets, TargetConfig config) {
    const unsigned t = part * 256 + sample;
    float sum = 0;
    if (part < update.parts && sample < 256 && t < update.support) {
        for (unsigned b = 0; b < BucketCount; ++b) {
            const unsigned r = update.rows[b];
            const int at = int(t) - int(b * config.bucket_stride);
            if (r != 0xffffffffu && at >= 0 && at < FFT_N)
                sum += buckets[(r * 2 + ear) * FFT_N + at] * (1.f / FFT_N);
        }
    }
    return sum;
}

template <unsigned BucketCount>
__device__ __forceinline__ void
target_fused(const Header* h, const Update* updates, const float* buckets, Complex* filters, TargetConfig config) {
    using FFT = room_fft::Forward;
    const unsigned target = blockIdx.y / 2, ear = blockIdx.y % 2;
    // This return is uniform across the block. Partial FFT groups still
    // participate in all shared-memory synchronization, with zero input.
    if (target >= h->updates)
        return;
    const Update& update = updates[target];
    const unsigned part = blockIdx.x * FFT::ffts_per_block + threadIdx.y;
    FFT::value_type data[FFT::storage_size] = {};
    auto* input_data = reinterpret_cast<FFT::input_type*>(data);
    for (unsigned i = 0; i < FFT::input_ept; ++i) {
        const unsigned k = threadIdx.x + i * FFT::stride;
        if (k < FFT::input_length) {
            input_data[i].x = target_time_sample<BucketCount>(update, ear, part, k * 2, buckets, config);
            input_data[i].y = target_time_sample<BucketCount>(update, ear, part, k * 2 + 1, buckets, config);
        }
    }
    extern __shared__ __align__(16) unsigned char fft_shared[];
    FFT().execute(data, reinterpret_cast<FFT::value_type*>(fft_shared));
    const auto* output_data = reinterpret_cast<const FFT::output_type*>(data);
    if (part < update.parts) {
        for (unsigned i = 0; i < FFT::output_ept; ++i) {
            const unsigned k = threadIdx.x + i * FFT::stride;
            if (k < 257) {
                const auto value = output_data[i];
                filters[(((update.group * TARGETS + update.slot) * 2 + ear) * config.parts + part) * 257 + k] = {
                    value.x, (k == 0 || k == 256) ? 0.f : value.y};
            }
        }
    }
}
// Preserve the fixed loop bound of the original per-room NVRTC compilation.
// Instantiate every supported bucket count; selection happens during prepare.
#define ROOM_TARGET_ENTRY(N)                                                                                           \
    extern "C" __global__ void session_target_fused_##N(                                                               \
        const Header* h, const Update* updates, const float* buckets, Complex* filters, TargetConfig config) {         \
        target_fused<N>(h, updates, buckets, filters, config);                                                         \
    }
#ifdef ROOM_GPU_FFT_LIBRARY
ROOM_TARGET_ENTRY(1)
ROOM_TARGET_ENTRY(2)
ROOM_TARGET_ENTRY(3)
ROOM_TARGET_ENTRY(4)
ROOM_TARGET_ENTRY(5)
ROOM_TARGET_ENTRY(6)
ROOM_TARGET_ENTRY(7)
ROOM_TARGET_ENTRY(8)
ROOM_TARGET_ENTRY(9)
ROOM_TARGET_ENTRY(10)
ROOM_TARGET_ENTRY(11)
ROOM_TARGET_ENTRY(12)
ROOM_TARGET_ENTRY(13)
ROOM_TARGET_ENTRY(14)
ROOM_TARGET_ENTRY(15)
ROOM_TARGET_ENTRY(16)
ROOM_TARGET_ENTRY(17)
ROOM_TARGET_ENTRY(18)
ROOM_TARGET_ENTRY(19)
ROOM_TARGET_ENTRY(20)
ROOM_TARGET_ENTRY(21)
ROOM_TARGET_ENTRY(22)
ROOM_TARGET_ENTRY(23)
ROOM_TARGET_ENTRY(24)
ROOM_TARGET_ENTRY(25)
ROOM_TARGET_ENTRY(26)
ROOM_TARGET_ENTRY(27)
ROOM_TARGET_ENTRY(28)
ROOM_TARGET_ENTRY(29)
ROOM_TARGET_ENTRY(30)
ROOM_TARGET_ENTRY(31)
ROOM_TARGET_ENTRY(32)
ROOM_TARGET_ENTRY(33)
ROOM_TARGET_ENTRY(34)
ROOM_TARGET_ENTRY(35)
ROOM_TARGET_ENTRY(36)
ROOM_TARGET_ENTRY(37)
ROOM_TARGET_ENTRY(38)
ROOM_TARGET_ENTRY(39)
ROOM_TARGET_ENTRY(40)
ROOM_TARGET_ENTRY(41)
ROOM_TARGET_ENTRY(42)
ROOM_TARGET_ENTRY(43)
ROOM_TARGET_ENTRY(44)
ROOM_TARGET_ENTRY(45)
ROOM_TARGET_ENTRY(46)
ROOM_TARGET_ENTRY(47)
ROOM_TARGET_ENTRY(48)
ROOM_TARGET_ENTRY(49)
ROOM_TARGET_ENTRY(50)
ROOM_TARGET_ENTRY(51)
ROOM_TARGET_ENTRY(52)
ROOM_TARGET_ENTRY(53)
ROOM_TARGET_ENTRY(54)
ROOM_TARGET_ENTRY(55)
ROOM_TARGET_ENTRY(56)
ROOM_TARGET_ENTRY(57)
ROOM_TARGET_ENTRY(58)
ROOM_TARGET_ENTRY(59)
ROOM_TARGET_ENTRY(60)
ROOM_TARGET_ENTRY(61)
ROOM_TARGET_ENTRY(62)
ROOM_TARGET_ENTRY(63)
ROOM_TARGET_ENTRY(64)
#else
extern "C" __global__ void session_target_fused(
    const Header* h, const Update* updates, const float* buckets, Complex* filters, TargetConfig config) {
    target_fused<BUCKETS>(h, updates, buckets, filters, config);
}
#endif
#undef ROOM_TARGET_ENTRY
#endif

#ifdef ROOM_GPU_FUSE_CONVOLVE
extern "C" __device__ __constant__ unsigned session_convolution_fft_traits[] = {room_fft::Inverse::block_dim.x,
                                                                                room_fft::Inverse::block_dim.y,
                                                                                room_fft::Inverse::shared_memory_size,
                                                                                room_fft::Inverse::ffts_per_block};

extern "C" __global__ void session_convolve_fused(
    const Header* h, const Info* info, const Task* tasks, const Complex* history, const Complex* filters, float* time) {
    using FFT = room_fft::Inverse;
    if (blockIdx.x * FFT::ffts_per_block >= h->tasks * 2)
        return;
    const unsigned row = blockIdx.x * FFT::ffts_per_block + threadIdx.y;
    const bool active = row / 2 < h->tasks;
    unsigned group = 0, head = 0, slot = 0, parts = 0;
    if (active) {
        const Task& task = tasks[row / 2];
        group = task.branch;
        head = info[group / 2].heads[group % 2];
        slot = task.slot;
        parts = task.parts;
    }
    FFT::value_type data[FFT::storage_size] = {};
    auto* input_data = reinterpret_cast<FFT::input_type*>(data);
    for (unsigned i = 0; i < FFT::input_ept; ++i) {
        const unsigned k = threadIdx.x + i * FFT::stride;
        if (k < 257) {
            Complex sum = {0, 0};
            for (unsigned p = 0; p < parts; ++p) {
                const unsigned at = (head + PARTS - p) % PARTS;
                const Complex x = history[(group * PARTS + at) * 257 + k];
                const Complex f = filters[(((group * TARGETS + slot) * 2 + row % 2) * PARTS + p) * 257 + k];
                sum = plus(sum, product(x, f));
            }
            input_data[i].x = sum.r;
            input_data[i].y = (k == 0 || k == 256) ? 0.f : sum.i;
        }
    }
    extern __shared__ __align__(16) unsigned char fft_shared[];
    FFT().execute(data, reinterpret_cast<FFT::value_type*>(fft_shared));
    const auto* output_data = reinterpret_cast<const FFT::output_type*>(data);
    if (active) {
        for (unsigned i = 0; i < FFT::output_ept; ++i) {
            const unsigned k = (threadIdx.x + i * FFT::stride) * 2;
            if (k >= 256 && k < 512) {
                // Keep the original unnormalised valid half. session_mix still
                // applies exactly the original 1/512 and finite-fade weights.
                time[row * 512 + k] = output_data[i].x;
                time[row * 512 + k + 1] = output_data[i].y;
            }
        }
    }
}
#endif
