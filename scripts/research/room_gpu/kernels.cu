// Independent feasibility kernels. Shapes are fixed by the exported fixture.
// No fast-math or reduced precision; path and partition sums have fixed order.
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
__device__ Complex add(Complex a, Complex b) {
    return {a.r + b.r, a.i + b.i};
}
__device__ Complex mul(Complex a, Complex b) {
    return {a.r * b.r - a.i * b.i, a.r * b.i + a.i * b.r};
}
__device__ Complex scale(Complex a, float b) {
    return {a.r * b, a.i * b};
}
__device__ Complex interpolate(const Complex* table, unsigned row, unsigned bin, float alpha) {
    Complex a = table[row * (FFT_N / 2 + 1) + bin], b = table[(row + 1) * (FFT_N / 2 + 1) + bin];
    return {a.r + (b.r - a.r) * alpha, a.i + (b.i - a.i) * alpha};
}
extern "C" __global__ void response(const Range* ranges,
                                    const Job* jobs,
                                    const Complex* bank,
                                    const Complex* fractional,
                                    const Complex* propagation,
                                    const Complex* materials,
                                    const Complex* phases,
                                    Complex* out) {
    unsigned bin = blockIdx.x * blockDim.x + threadIdx.x;
    if (bin >= FFT_N / 2 + 1)
        return;
    unsigned row = blockIdx.y, ear = row % 2;
    Range range = ranges[row / 2];
    Complex sum = {0, 0};
    for (unsigned p = 0; p < range.count; ++p) {
        const Job& job = jobs[range.first + p];
        Complex h = {0, 0};
        unsigned count = (job.counts >> (8 * ear)) & 255;
        for (unsigned t = 0; t < count; ++t) {
            Term term = job.terms[ear * 6 + t];
            Complex kernel = interpolate(fractional, term.fraction, bin, term.alpha);
            Complex v = mul(bank[term.row * (FFT_N / 2 + 1) + bin], kernel);
            v = mul(v, phases[(term.shift * bin) & (FFT_N - 1)]);
            h = add(h, scale(v, term.weight));
        }
        if (bin == 0 || bin == FFT_N / 2)
            h.i = 0;
        Complex transfer = interpolate(propagation, job.fraction, bin, job.alpha);
        transfer = mul(transfer, phases[(job.shift * bin) & (FFT_N - 1)]);
        transfer = scale(mul(transfer, materials[job.material * (FFT_N / 2 + 1) + bin]), job.gain);
        sum = add(sum, mul(h, transfer));
    }
    out[row * (FFT_N / 2 + 1) + bin] = sum;
}
extern "C" __global__ void assemble(const float* buckets, float* impulse) {
    unsigned t = blockIdx.x * blockDim.x + threadIdx.x, channel = blockIdx.y;
    if (t >= TAPS)
        return;
    unsigned voice = channel / 4, branch = (channel % 4) / 2, ear = channel % 2;
    float sum = 0;
    for (unsigned b = 0; b < BUCKETS; ++b) {
        int index = int(t) - int(b * BUCKET_STRIDE);
        if (index >= 0 && index < FFT_N) {
            unsigned row = ((voice * 2 + branch) * BUCKETS + b) * 2 + ear;
            sum += buckets[row * FFT_N + index] * (1.f / FFT_N);
        }
    }
    impulse[channel * TAPS + t] = sum;
}
extern "C" __global__ void pack_filters(const float* impulse, float* windows) {
    unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= VOICES * 4 * PARTS * 512)
        return;
    unsigned row = i / 512, offset = i % 512, channel = row / PARTS, part = row % PARTS;
    unsigned t = part * 256 + offset;
    windows[i] = (offset < 256 && t < TAPS) ? impulse[channel * TAPS + t] : 0.f;
}
extern "C" __global__ void install_filters(const Complex* fresh, Complex* filters, const unsigned* step) {
    unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < VOICES * 4 * PARTS * 257)
        filters[(*step % TARGETS) * (VOICES * 4 * PARTS * 257) + i] = fresh[i];
}
extern "C" __global__ void pack_input(const float* input, float* previous, float* windows) {
    unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= VOICES * 256)
        return;
    unsigned voice = i / 256, offset = i % 256;
    windows[voice * 512 + offset] = previous[i];
    windows[voice * 512 + 256 + offset] = input[i];
    previous[i] = input[i];
}
extern "C" __global__ void store_history(const Complex* input, Complex* history, const unsigned* step) {
    unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < VOICES * 257)
        history[(i / 257 * PARTS + *step % PARTS) * 257 + i % 257] = input[i];
}
extern "C" __global__ void
convolve(const Complex* history, const Complex* filters, Complex* output, const unsigned* step) {
    unsigned b = blockIdx.x * blockDim.x + threadIdx.x, row = blockIdx.y;
    if (b >= 257)
        return;
    unsigned channel = row / TARGETS, target = row % TARGETS, voice = channel / 4;
    Complex sum = {0, 0};
    for (unsigned p = 0; p < PARTS; ++p) {
        unsigned at = (*step % PARTS + PARTS - p) % PARTS;
        Complex x = history[(voice * PARTS + at) * 257 + b];
        Complex h = filters[((target * VOICES * 4 + channel) * PARTS + p) * 257 + b];
        sum = add(sum, mul(x, h));
    }
    output[row * 257 + b] = sum;
}
// Uniform weights exercise the requested active-target occupancy. This is a
// capacity probe, not the product's chronological 10 ms finite fade.
extern "C" __global__ void mix(const float* convolved, float* output) {
    unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= 512)
        return;
    unsigned ear = i / 256, t = i % 256;
    float sum = 0;
    for (unsigned voice = 0; voice < VOICES; ++voice)
        for (unsigned branch = 0; branch < 2; ++branch)
            for (unsigned target = 0; target < TARGETS; ++target) {
                unsigned row = ((voice * 4 + branch * 2 + ear) * TARGETS + target);
                sum += convolved[row * 512 + 256 + t] * (1.f / (512 * TARGETS));
            }
    output[i] = sum;
}
extern "C" __global__ void copy_audio(const float* input, float* output) {
    unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < 512)
        output[i] = input[i % 256];
}
extern "C" __global__ void advance(unsigned* step) {
    if (threadIdx.x == 0)
        ++*step;
}
