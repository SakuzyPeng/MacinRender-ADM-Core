// Standalone MSVC + CUDA/cuFFT feasibility executable; no production linkage.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <cufft.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <nvrtc.h>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
void runtime(cudaError_t result, const std::source_location& where = std::source_location::current()) {
    if (result != cudaSuccess)
        throw std::runtime_error(std::string(cudaGetErrorString(result)) + " in " + where.function_name() + ":" +
                                 std::to_string(where.line()));
}
void driver(CUresult result) {
    if (result != CUDA_SUCCESS) {
        const char* message = nullptr;
        cuGetErrorString(result, &message);
        throw std::runtime_error(message ? message : "CUDA driver error");
    }
}
void fft_check(cufftResult result) {
    if (result != CUFFT_SUCCESS)
        throw std::runtime_error("cuFFT error " + std::to_string(result));
}
struct Device {
    void* data = nullptr;
    size_t bytes;
    explicit Device(size_t count) : bytes(count) { runtime(cudaMalloc(&data, bytes)); }
    Device(const Device&) = delete;
    ~Device() { cudaFree(data); }
};
struct Pinned {
    void* data = nullptr;
    size_t bytes;
    explicit Pinned(size_t count) : bytes(count) { runtime(cudaMallocHost(&data, bytes)); }
    Pinned(const Pinned&) = delete;
    ~Pinned() { cudaFreeHost(data); }
};
struct Plan {
    cufftHandle value = 0;
    Plan(int n, cufftType type, int count, cudaStream_t stream) {
        fft_check(cufftPlan1d(&value, n, type, count));
        fft_check(cufftSetStream(value, stream));
    }
    ~Plan() { cufftDestroy(value); }
};
std::vector<char> read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Cannot read " + path.string());
    const auto size = file.tellg();
    if (size < 0)
        throw std::runtime_error("Invalid input size");
    std::vector<char> bytes(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(bytes.data(), size))
        throw std::runtime_error("Incomplete input");
    return bytes;
}
void load(Device& destination, const fs::path& path) {
    auto bytes = read(path);
    if (bytes.size() != destination.bytes)
        throw std::runtime_error("Unexpected shape: " + path.string());
    runtime(cudaMemcpy(destination.data, bytes.data(), bytes.size(), cudaMemcpyHostToDevice));
}
std::vector<float> read_floats(const fs::path& path) {
    const auto bytes = read(path);
    if (bytes.size() % sizeof(float) != 0) {
        throw std::runtime_error("Invalid float payload size");
    }
    std::vector<float> values(bytes.size() / sizeof(float));
    std::memcpy(values.data(), bytes.data(), bytes.size());
    return values;
}
struct Config {
    int version, rate, voices, n, taps, bucket_stride, buckets, jobs, cases, rows, materials;
    explicit Config(const fs::path& path) {
        std::ifstream f(path);
        if (!(f >> version >> rate >> voices >> n >> taps >> bucket_stride >> buckets >> jobs >> cases >> rows >>
              materials) ||
            version != 1 || rate != 96000 || voices != 32 || n != 4096 || taps <= 0 || taps > 32768 || buckets <= 0 ||
            buckets > 64 || jobs <= 0 || jobs > 16384 || cases != 16 || rows <= 0 || rows > 65536 || materials <= 0 ||
            materials > 1024)
            throw std::runtime_error("Unsupported fixture shape");
    }
};
struct Module {
    CUmodule value = nullptr;
    Module(const fs::path& source, const Config& c, int targets, int major, int minor) {
        auto text = read(source);
        text.push_back(0);
        nvrtcProgram program;
        auto check = [](nvrtcResult r) {
            if (r != NVRTC_SUCCESS)
                throw std::runtime_error(nvrtcGetErrorString(r));
        };
        check(nvrtcCreateProgram(&program, text.data(), "room_gpu.cu", 0, nullptr, nullptr));
        std::vector<std::string> options = {"--std=c++14",
                                            "--fmad=true",
                                            "--ftz=false",
                                            "--prec-div=true",
                                            "--prec-sqrt=true",
                                            "--gpu-architecture=compute_" + std::to_string(major) +
                                                std::to_string(minor),
                                            "-DFFT_N=" + std::to_string(c.n),
                                            "-DVOICES=" + std::to_string(c.voices),
                                            "-DTAPS=" + std::to_string(c.taps),
                                            "-DBUCKETS=" + std::to_string(c.buckets),
                                            "-DBUCKET_STRIDE=" + std::to_string(c.bucket_stride),
                                            "-DPARTS=" + std::to_string((c.taps + 255) / 256),
                                            "-DTARGETS=" + std::to_string(targets)};
        std::vector<const char*> pointers;
        std::transform(options.begin(), options.end(), std::back_inserter(pointers), [](const auto& option) {
            return option.c_str();
        });
        const auto compiled = nvrtcCompileProgram(program, static_cast<int>(pointers.size()), pointers.data());
        size_t log_size = 0;
        check(nvrtcGetProgramLogSize(program, &log_size));
        std::vector<char> log(log_size);
        check(nvrtcGetProgramLog(program, log.data()));
        if (compiled != NVRTC_SUCCESS) {
            nvrtcDestroyProgram(&program);
            throw std::runtime_error(log.data());
        }
        size_t size;
        check(nvrtcGetPTXSize(program, &size));
        std::vector<char> ptx(size);
        check(nvrtcGetPTX(program, ptx.data()));
        check(nvrtcDestroyProgram(&program));
        driver(cuModuleLoadData(&value, ptx.data()));
    }
    ~Module() { cuModuleUnload(value); }
    CUfunction function(const char* name) {
        CUfunction f;
        driver(cuModuleGetFunction(&f, value, name));
        return f;
    }
};
struct Error {
    double l2 = 0, peak = 0;
    bool passed = false;
};
Error difference(const float* actual, const float* expected, size_t count) {
    double energy = 0, error = 0, peak = 0, maximum = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!std::isfinite(actual[i]) || !std::isfinite(expected[i]))
            throw std::runtime_error("Nonfinite result");
        const double a = actual[i], b = expected[i], d = a - b;
        energy += b * b;
        error += d * d;
        peak = std::max(peak, std::abs(b));
        maximum = std::max(maximum, std::abs(d));
    }
    Error result{energy ? std::sqrt(error / energy) : error, peak ? maximum / peak : maximum, false};
    result.passed = result.l2 <= 2e-5 && result.peak <= 2e-4 && (energy != 0 || error == 0);
    return result;
}

int main(int argc, char** argv) try {
    if (argc < 7 || argc > 9)
        throw std::runtime_error("usage: probe FIXTURE KERNELS transfer|response|resident TARGETS CALLS graph|launch "
                                 "[both|ramps|distance-ramps] [0|1 pacing]");
    const fs::path fixture = argv[1];
    const std::string route = argv[3], submission = argv[6];
    const std::string scenario = argc >= 8 ? argv[7] : "both";
    const bool paced = argc >= 9 && std::string(argv[8]) == "1";
    const int targets = std::stoi(argv[4]), calls = std::stoi(argv[5]);
    if ((route != "transfer" && route != "response" && route != "resident") || targets < 1 || targets > 16 ||
        calls < 0 || calls > 1024 || (submission != "graph" && submission != "launch") ||
        (scenario != "both" && scenario != "ramps" && scenario != "distance-ramps"))
        throw std::runtime_error("Invalid options");
    const Config c(fixture / "config.txt");
    runtime(cudaSetDevice(0));
    runtime(cudaFree(nullptr));
    cudaDeviceProp props{};
    runtime(cudaGetDeviceProperties(&props, 0));
    cudaStream_t stream;
    runtime(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
    const size_t bins = c.n / 2 + 1, parts = (c.taps + 255) / 256, channels = c.voices * 4;
    const size_t range_count = c.voices * 2 * c.buckets, response_rows = range_count * 2;
    const size_t impulse_count = channels * c.taps, filter_count = channels * parts * 257;
    Module module(argv[2], c, targets, props.major, props.minor);
    Device bank(c.rows * bins * 8), fractional(1025 * bins * 8), propagation(1025 * bins * 8),
        materials(c.materials * bins * 8), phases(c.n * 8);
    load(bank, fixture / "bank.bin");
    load(fractional, fixture / "fraction.bin");
    load(propagation, fixture / "propagation.bin");
    load(materials, fixture / "materials.bin");
    load(phases, fixture / "phase.bin");
    Device jobs(c.jobs * 264), ranges(range_count * 8), spectrum(response_rows * bins * 8),
        bucket_time(response_rows * c.n * 4);
    Device impulse(impulse_count * 4), packed(channels * parts * 512 * 4), fresh(filter_count * 8),
        filters(targets * filter_count * 8);
    Device input(c.voices * 256 * 4), previous(c.voices * 256 * 4), input_windows(c.voices * 512 * 4),
        input_fft(c.voices * 257 * 8);
    Device history(c.voices * parts * 257 * 8), convolved(channels * targets * 257 * 8),
        convolved_time(channels * targets * 512 * 4), output(512 * 4), step(4);
    Pinned host_jobs(2 * jobs.bytes), host_ranges(2 * ranges.bytes), host_input(2 * input.bytes);
    Pinned host_output(2 * (route == "response" ? impulse.bytes : output.bytes));
    Plan response_plan(c.n, CUFFT_C2R, static_cast<int>(response_rows), stream);
    Plan target_plan(512, CUFFT_R2C, static_cast<int>(channels * parts), stream);
    Plan input_plan(512, CUFFT_R2C, c.voices, stream);
    Plan output_plan(512, CUFFT_C2R, static_cast<int>(channels * targets), stream);
    auto all_jobs = read(fixture / "jobs.bin"), all_ranges = read(fixture / "ranges.bin");
    const auto all_reference = read_floats(fixture / "reference.bin");
    if (all_jobs.size() != jobs.bytes * c.cases || all_ranges.size() != ranges.bytes * c.cases ||
        all_reference.size() != impulse_count * c.cases)
        throw std::runtime_error("Snapshot shape mismatch");
    CUfunction response = module.function("response"), assemble = module.function("assemble"),
               pack_filters = module.function("pack_filters"), install = module.function("install_filters"),
               pack_input = module.function("pack_input"), store_history = module.function("store_history"),
               convolve = module.function("convolve"), mix = module.function("mix"),
               advance = module.function("advance"), copy_audio = module.function("copy_audio");
    auto launch = [&](CUfunction f, unsigned x, unsigned y, std::initializer_list<void*> arguments) {
        std::array<void*, 12> a{};
        if (arguments.size() > a.size())
            throw std::runtime_error("Too many kernel arguments");
        std::copy(arguments.begin(), arguments.end(), a.begin());
        driver(cuLaunchKernel(f, x, y, 1, 128, 1, 1, 0, reinterpret_cast<CUstream>(stream), a.data(), nullptr));
    };
    auto groups = [](size_t n) { return static_cast<unsigned>((n + 127) / 128); };
    auto build_response = [&] {
        launch(response,
               groups(bins),
               static_cast<unsigned>(response_rows),
               {&ranges.data,
                &jobs.data,
                &bank.data,
                &fractional.data,
                &propagation.data,
                &materials.data,
                &phases.data,
                &spectrum.data});
        fft_check(cufftExecC2R(
            response_plan.value, static_cast<cufftComplex*>(spectrum.data), static_cast<float*>(bucket_time.data)));
        launch(assemble, groups(c.taps), static_cast<unsigned>(channels), {&bucket_time.data, &impulse.data});
    };
    // Arithmetic validation is outside all timed calls.
    std::vector<float> actual(impulse_count);
    double worst_l2 = 0, worst_peak = 0;
    for (int i = 0; i < c.cases; ++i) {
        runtime(
            cudaMemcpyAsync(jobs.data, all_jobs.data() + i * jobs.bytes, jobs.bytes, cudaMemcpyHostToDevice, stream));
        runtime(cudaMemcpyAsync(
            ranges.data, all_ranges.data() + i * ranges.bytes, ranges.bytes, cudaMemcpyHostToDevice, stream));
        build_response();
        runtime(cudaMemcpyAsync(actual.data(), impulse.data, impulse.bytes, cudaMemcpyDeviceToHost, stream));
        runtime(cudaStreamSynchronize(stream));
        for (size_t channel = 0; channel < channels; ++channel) {
            const auto e = difference(
                actual.data() + channel * c.taps, all_reference.data() + i * impulse_count + channel * c.taps, c.taps);
            worst_l2 = std::max(worst_l2, e.l2);
            worst_peak = std::max(worst_peak, e.peak);
            if (!e.passed) {
                throw std::runtime_error("Response numerical gate failed: case " + std::to_string(i) + " plane " +
                                         std::to_string(channel) + ": " + std::to_string(e.l2) + " / " +
                                         std::to_string(e.peak));
            }
        }
    }
    // Seed every active target from the same real response and full input history.
    runtime(cudaMemcpyAsync(jobs.data, all_jobs.data(), jobs.bytes, cudaMemcpyHostToDevice, stream));
    runtime(cudaMemcpyAsync(ranges.data, all_ranges.data(), ranges.bytes, cudaMemcpyHostToDevice, stream));
    build_response();
    launch(pack_filters, groups(channels * parts * 512), 1, {&impulse.data, &packed.data});
    fft_check(
        cufftExecR2C(target_plan.value, static_cast<float*>(packed.data), static_cast<cufftComplex*>(fresh.data)));
    for (int t = 0; t < targets; ++t)
        runtime(cudaMemcpyAsync(static_cast<char*>(filters.data) + t * fresh.bytes,
                                fresh.data,
                                fresh.bytes,
                                cudaMemcpyDeviceToDevice,
                                stream));
    runtime(cudaStreamSynchronize(stream));
    load(history, fixture / "history.bin");
    load(previous, fixture / "previous.bin");
    runtime(cudaMemset(step.data, 0, 4));
    auto prepare = [&](int first_case, int sample) {
        for (int half = 0; half < 2; ++half) {
            const int which = first_case < 0 ? 0 : (first_case + half) % c.cases;
            std::memcpy(static_cast<char*>(host_jobs.data) + half * jobs.bytes,
                        all_jobs.data() + which * jobs.bytes,
                        jobs.bytes);
            std::memcpy(static_cast<char*>(host_ranges.data) + half * ranges.bytes,
                        all_ranges.data() + which * ranges.bytes,
                        ranges.bytes);
            auto* audio = static_cast<float*>(host_input.data) + half * c.voices * 256;
            for (int voice = 0; voice < c.voices; ++voice)
                for (int i = 0; i < 256; ++i)
                    audio[voice * 256 + i] =
                        static_cast<float>((sample + half * 256 + i + voice * 61) % 179 - 89) * 0.00001f;
        }
    };
    auto body = [&] {
        for (int half = 0; half < 2; ++half) {
            if (route != "transfer") {
                runtime(cudaMemcpyAsync(jobs.data,
                                        static_cast<char*>(host_jobs.data) + half * jobs.bytes,
                                        jobs.bytes,
                                        cudaMemcpyHostToDevice,
                                        stream));
                runtime(cudaMemcpyAsync(ranges.data,
                                        static_cast<char*>(host_ranges.data) + half * ranges.bytes,
                                        ranges.bytes,
                                        cudaMemcpyHostToDevice,
                                        stream));
                build_response();
            }
            if (route == "response") {
                runtime(cudaMemcpyAsync(static_cast<char*>(host_output.data) + half * impulse.bytes,
                                        impulse.data,
                                        impulse.bytes,
                                        cudaMemcpyDeviceToHost,
                                        stream));
            } else {
                runtime(cudaMemcpyAsync(input.data,
                                        static_cast<char*>(host_input.data) + half * input.bytes,
                                        input.bytes,
                                        cudaMemcpyHostToDevice,
                                        stream));
                if (route == "resident") {
                    launch(pack_filters, groups(channels * parts * 512), 1, {&impulse.data, &packed.data});
                    fft_check(cufftExecR2C(
                        target_plan.value, static_cast<float*>(packed.data), static_cast<cufftComplex*>(fresh.data)));
                    launch(install, groups(filter_count), 1, {&fresh.data, &filters.data, &step.data});
                    launch(pack_input, groups(c.voices * 256), 1, {&input.data, &previous.data, &input_windows.data});
                    fft_check(cufftExecR2C(input_plan.value,
                                           static_cast<float*>(input_windows.data),
                                           static_cast<cufftComplex*>(input_fft.data)));
                    launch(store_history, groups(c.voices * 257), 1, {&input_fft.data, &history.data, &step.data});
                    launch(convolve,
                           groups(257),
                           static_cast<unsigned>(channels * targets),
                           {&history.data, &filters.data, &convolved.data, &step.data});
                    fft_check(cufftExecC2R(output_plan.value,
                                           static_cast<cufftComplex*>(convolved.data),
                                           static_cast<float*>(convolved_time.data)));
                    launch(mix, groups(512), 1, {&convolved_time.data, &output.data});
                } else
                    launch(copy_audio, groups(512), 1, {&input.data, &output.data});
                runtime(cudaMemcpyAsync(static_cast<char*>(host_output.data) + half * output.bytes,
                                        output.data,
                                        output.bytes,
                                        cudaMemcpyDeviceToHost,
                                        stream));
            }
            launch(advance, 1, 1, {&step.data});
        }
    };
    prepare(-1, 0);
    body();
    runtime(cudaStreamSynchronize(stream));
    Error audio_error{0, 0, true};
    if (route == "resident") {
        std::vector<float> audio(1024);
        for (int half = 0; half < 2; ++half)
            for (int ear = 0; ear < 2; ++ear)
                std::memcpy(audio.data() + ear * 512 + half * 256,
                            static_cast<float*>(host_output.data) + half * 512 + ear * 256,
                            256 * 4);
        const auto expected = read_floats(fixture / "audio-reference.bin");
        if (expected.size() != 1024)
            throw std::runtime_error("Audio reference shape mismatch");
        audio_error = difference(audio.data(), expected.data(), 1024);
        if (!audio_error.passed)
            throw std::runtime_error("Convolution numerical gate failed: " + std::to_string(audio_error.l2) + " / " +
                                     std::to_string(audio_error.peak));
    }
    cudaGraph_t graph = nullptr;
    cudaGraphExec_t execution = nullptr;
    if (submission == "graph") {
        runtime(cudaStreamBeginCapture(stream, cudaStreamCaptureModeThreadLocal));
        body();
        runtime(cudaStreamEndCapture(stream, &graph));
        runtime(cudaGraphInstantiate(&execution, graph, 0));
    }
    auto run = [&] {
        if (execution)
            runtime(cudaGraphLaunch(execution, stream));
        else
            body();
        runtime(cudaStreamSynchronize(stream));
    };
    std::vector<double> elapsed;
    elapsed.reserve(calls);
    std::vector<char> consumed(host_output.bytes);
    for (int i = 0; i < calls + 32; ++i) {
        const auto start = Clock::now();
        prepare((scenario == "distance-ramps" ? 8 : 0) + (i * 2) % (scenario == "both" ? 16 : 8), i * 512);
        run();
        std::memcpy(consumed.data(), host_output.data, consumed.size());
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        if (i >= 32)
            elapsed.push_back(ms);
        if (paced)
            std::this_thread::sleep_until(start + std::chrono::nanoseconds(5333333));
    }
    size_t free_bytes, total_bytes;
    double checksum = 0;
    for (size_t i = 0; i < consumed.size(); i += sizeof(float)) {
        float sample;
        std::memcpy(&sample, consumed.data() + i, sizeof(sample));
        if (!std::isfinite(sample)) {
            throw std::runtime_error("Nonfinite timed output");
        }
        checksum += sample;
    }
    runtime(cudaMemGetInfo(&free_bytes, &total_bytes));
    std::sort(elapsed.begin(), elapsed.end());
    const double mean = calls ? std::accumulate(elapsed.begin(), elapsed.end(), 0.) / calls : 0.;
    const auto misses = std::count_if(elapsed.begin(), elapsed.end(), [](double ms) { return ms > 512000. / 96000.; });
    std::cout << std::setprecision(12) << "{\"schema\":\"mradm.room-gpu-feasibility\",\"version\":1,\"device\":\""
              << props.name << "\",\"route\":\"" << route << "\",\"submission\":\"" << submission
              << "\",\"calls\":" << calls << ",\"scenario\":\"" << scenario
              << "\",\"paced\":" << (paced ? "true" : "false") << ",\"sources\":" << c.voices
              << ",\"sample_rate\":" << c.rate << ",\"call_frames\":512,\"active_targets\":" << targets
              << ",\"response_updates_per_call\":64,\"host_preparation_precomputed\":true,\"full_session\":false"
              << ",\"response_relative_l2\":" << worst_l2 << ",\"response_maximum_over_peak\":" << worst_peak
              << ",\"audio_relative_l2\":" << audio_error.l2 << ",\"audio_maximum_over_peak\":" << audio_error.peak
              << ",\"audio_checked\":" << (route == "resident" ? "true" : "false") << ",\"mean_ms\":" << mean
              << ",\"p99_ms\":" << (calls ? elapsed[std::min(calls - 1, calls * 99 / 100)] : 0.)
              << ",\"maximum_ms\":" << (calls ? elapsed.back() : 0.) << ",\"deadline_misses\":" << misses
              << ",\"gpu_total_bytes\":" << total_bytes << ",\"gpu_used_bytes_observed\":" << total_bytes - free_bytes
              << ",\"metadata_h2d_bytes_per_call\":" << (route == "transfer" ? 0 : 2 * (jobs.bytes + ranges.bytes))
              << ",\"audio_h2d_bytes_per_call\":" << (route == "response" ? 0 : 2 * input.bytes)
              << ",\"d2h_bytes_per_call\":" << host_output.bytes << ",\"output_checksum\":" << checksum << "}\n";
    if (execution)
        cudaGraphExecDestroy(execution);
    if (graph)
        cudaGraphDestroy(graph);
    cudaStreamDestroy(stream);
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
