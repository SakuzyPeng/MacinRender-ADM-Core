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
    unsigned bin = blockIdx.x * blockDim.x + threadIdx.x, row = blockIdx.y;
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
