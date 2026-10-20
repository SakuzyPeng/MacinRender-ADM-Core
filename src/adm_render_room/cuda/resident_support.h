#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <cufft.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nvrtc.h>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef ROOM_GPU_PRECOMPILED_TARGET
#include "runtime_files.h"
#endif

using Clock = std::chrono::steady_clock;
// CUDA resource handles become invalid after a context reset. Keep its unique
// ID so a recreated primary context cannot make stale handles appear usable.
struct Context {
    CUcontext value = nullptr;
    unsigned long long id = 0;
    Context() {
        if (cuCtxGetCurrent(&value) != CUDA_SUCCESS || !value || cuCtxGetId(value, &id) != CUDA_SUCCESS)
            throw std::runtime_error("Cannot identify ROOM CUDA context");
    }
    bool valid() const noexcept {
        unsigned long long current = 0;
        return cuCtxGetId(value, &current) == CUDA_SUCCESS && current == id;
    }
    void require() const {
        if (!valid())
            throw std::runtime_error("ROOM CUDA context was lost; release and recreate all affected sessions");
    }
};
struct StreamOwner {
    Context context;
    cudaStream_t value = nullptr;
    ~StreamOwner() {
        if (value && context.valid())
            cudaStreamDestroy(value);
    }
};
namespace room_cuda {
inline std::atomic<std::uint64_t> banks{0}, engines{0}, late_engines{0}, device_bytes{0}, pinned_bytes{0};
struct Counted {
    std::atomic<std::uint64_t>& value;
    explicit Counted(std::atomic<std::uint64_t>& v) : value(v) { ++value; }
    ~Counted() { --value; }
};
} // namespace room_cuda

inline void runtime(cudaError_t result, const std::source_location& where = std::source_location::current()) {
    if (result != cudaSuccess)
        throw std::runtime_error(std::string(cudaGetErrorString(result)) + " in " + where.function_name());
}
inline void driver(CUresult result) {
    if (result != CUDA_SUCCESS) {
        const char* message = nullptr;
        cuGetErrorString(result, &message);
        throw std::runtime_error(message ? message : "CUDA driver error");
    }
}
inline void fft_check(cufftResult result) {
    if (result != CUFFT_SUCCESS)
        throw std::runtime_error("cuFFT error " + std::to_string(result));
}
template <class Work>
void capture_graph(cudaStream_t stream, Work&& work, cudaGraph_t& graph, cudaGraphExec_t& execution) {
    runtime(cudaStreamBeginCapture(stream, cudaStreamCaptureModeThreadLocal));
    try {
        work();
        runtime(cudaStreamEndCapture(stream, &graph));
        runtime(cudaGraphInstantiate(&execution, graph, 0));
    } catch (...) {
        cudaStreamCaptureStatus status = cudaStreamCaptureStatusNone;
        if (cudaStreamIsCapturing(stream, &status) == cudaSuccess && status != cudaStreamCaptureStatusNone) {
            cudaGraph_t aborted = nullptr;
            cudaStreamEndCapture(stream, &aborted);
            if (aborted)
                cudaGraphDestroy(aborted);
        }
        throw;
    }
}
struct Device {
    Context context;
    void* data = nullptr;
    size_t bytes;
    bool owned = true;
    explicit Device(size_t count) : bytes(count) {
        runtime(cudaMalloc(&data, bytes));
        room_cuda::device_bytes += bytes;
    }
    Device(void* view, size_t count) : data(view), bytes(count), owned(false) {}
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    ~Device() {
        if (owned && data) {
            if (context.valid())
                cudaFree(data);
            room_cuda::device_bytes -= bytes;
        }
    }
};
struct Pinned {
    Context context;
    void* data = nullptr;
    size_t bytes;
    explicit Pinned(size_t count) : bytes(count) {
        runtime(cudaMallocHost(&data, bytes));
        room_cuda::pinned_bytes += bytes;
    }
    Pinned(const Pinned&) = delete;
    Pinned& operator=(const Pinned&) = delete;
    ~Pinned() {
        if (data) {
            if (context.valid())
                cudaFreeHost(data);
            room_cuda::pinned_bytes -= bytes;
        }
    }
};
struct Plan {
    Context context;
    cufftHandle value = 0;
    Plan(int n, cufftType type, int count, cudaStream_t stream) {
        try {
            fft_check(cufftPlan1d(&value, n, type, count));
            fft_check(cufftSetStream(value, stream));
        } catch (...) {
            if (value)
                cufftDestroy(value);
            throw;
        }
    }
    ~Plan() {
        if (value && context.valid())
            cufftDestroy(value);
    }
    Plan(const Plan&) = delete;
    Plan& operator=(const Plan&) = delete;
};
struct Config {
    int rate = 96000, voices = 32, n = 4096, taps = 256, bucket_stride = 256, buckets = 1, rows = 0, materials = 1;
};
struct Module {
    Context context;
    CUmodule value = nullptr;
#ifdef ROOM_GPU_PRECOMPILED_TARGET
    explicit Module(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input)
            throw std::runtime_error(
                "ROOM CUDA target FFT device code is missing or this GPU architecture is unsupported");
        const auto size = input.tellg();
        if (size <= 0 || size > 64 * 1024 * 1024)
            throw std::runtime_error("Invalid ROOM CUDA target FFT device code");
        input.seekg(0);
        std::vector<char> ptx(static_cast<size_t>(size) + 1);
        input.read(ptx.data(), static_cast<std::streamsize>(size));
        if (!input)
            throw std::runtime_error("Cannot read ROOM CUDA target FFT device code");
        try {
            driver(cuModuleLoadData(&value, ptx.data()));
        } catch (...) {
            if (value)
                cuModuleUnload(value);
            throw;
        }
    }
#endif
    Module(const std::string& source, const Config& c, int targets, int major, int minor, bool fusion = false) {
        nvrtcProgram program = nullptr;
        auto check = [](nvrtcResult r) {
            if (r != NVRTC_SUCCESS)
                throw std::runtime_error(nvrtcGetErrorString(r));
        };
        try {
            check(nvrtcCreateProgram(&program, source.c_str(), "room_cuda.cu", 0, nullptr, nullptr));
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
#if defined(ROOM_GPU_FUSE_CONVOLVE) || (defined(ROOM_GPU_FUSE_TARGET) && !defined(ROOM_GPU_PRECOMPILED_TARGET))
            if (fusion) {
                options[0] = "--std=c++17";
                options.emplace_back("--device-as-default-execution-space");
                options.emplace_back("-DCUFFTDX_DISABLE_CUTLASS_DEPENDENCY");
                options.emplace_back("--include-path=" ROOM_CUDA_MATHDX_INCLUDE);
                options.emplace_back("--include-path=" ROOM_CUDA_TOOLKIT_INCLUDE);
                options.emplace_back("-DROOM_FFT_SM=" + std::to_string(major * 100 + minor * 10));
#if defined(ROOM_GPU_FUSE_TARGET) && !defined(ROOM_GPU_PRECOMPILED_TARGET)
                options.emplace_back("-DROOM_GPU_FUSE_TARGET");
#endif
#ifdef ROOM_GPU_FUSE_CONVOLVE
                options.emplace_back("-DROOM_GPU_FUSE_CONVOLVE");
#endif
            }
#else
            (void) fusion;
#endif
#ifdef ROOM_GPU_FREQUENCY_MAJOR
            options.emplace_back("-DROOM_GPU_FREQUENCY_MAJOR");
#endif
            std::vector<const char*> pointers;
            for (const auto& option : options)
                pointers.push_back(option.c_str());
            const auto compiled = nvrtcCompileProgram(program, static_cast<int>(pointers.size()), pointers.data());
            if (compiled != NVRTC_SUCCESS) {
                size_t bytes = 0;
                check(nvrtcGetProgramLogSize(program, &bytes));
                std::vector<char> log(bytes + 1);
                check(nvrtcGetProgramLog(program, log.data()));
                throw std::runtime_error(log.data());
            }
            size_t bytes = 0;
            check(nvrtcGetPTXSize(program, &bytes));
            std::vector<char> ptx(bytes);
            check(nvrtcGetPTX(program, ptx.data()));
            check(nvrtcDestroyProgram(&program));
            driver(cuModuleLoadData(&value, ptx.data()));
        } catch (...) {
            if (program)
                nvrtcDestroyProgram(&program);
            if (value)
                cuModuleUnload(value);
            throw;
        }
    }
    ~Module() {
        if (value && context.valid())
            cuModuleUnload(value);
    }
    CUfunction function(const char* name) const {
        CUfunction result;
        driver(cuModuleGetFunction(&result, value, name));
        return result;
    }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
};

// This descriptor is private to the optional native plugin, not adm/c_api.h.
struct RoomCudaBankDesc {
    std::uint32_t abi, size, rate, fft, rows;
};
struct RoomCudaStats {
    std::uint64_t banks, engines, late_engines, device_bytes, pinned_bytes;
};
struct ResidentBank {
    Context context;
    room_cuda::Counted active{room_cuda::banks};
    Config config;
    std::string source;
    Device bank, fractional, phases;
    std::array<size_t, 3> written{};
    bool sealed = false;
    cudaDeviceProp properties{};
    static Config validate(const RoomCudaBankDesc& desc) {
        if (desc.abi != 1 || desc.size != sizeof(RoomCudaBankDesc) || desc.rate != 96000 || desc.fft != 4096 ||
            !desc.rows || desc.rows > 65536)
            throw std::runtime_error("Unsupported resident ROOM CUDA resource shape");
        Config c;
        c.rows = static_cast<int>(desc.rows);
        return c;
    }
    ResidentBank(const RoomCudaBankDesc& desc, const char* text, size_t length)
        : config(validate(desc)), source(text, length), bank(size_t(desc.rows) * (desc.fft / 2 + 1) * 8),
          fractional(size_t(1025) * (desc.fft / 2 + 1) * 8), phases(size_t(desc.fft) * 8) {
        runtime(cudaGetDeviceProperties(&properties, 0));
    }
    void upload(unsigned table, size_t offset, const float* data, size_t count) {
        Device* arrays[] = {&bank, &fractional, &phases};
        if (sealed || table >= 3 || !data || !count || offset != written[table] || offset > arrays[table]->bytes / 4 ||
            count > arrays[table]->bytes / 4 - offset)
            throw std::runtime_error("Invalid resident ROOM CUDA upload window");
        runtime(cudaSetDevice(0));
        runtime(cudaMemcpy(static_cast<float*>(arrays[table]->data) + offset, data, count * 4, cudaMemcpyHostToDevice));
        written[table] += count;
    }
    void seal() {
        if (sealed || written != std::array<size_t, 3>{bank.bytes / 4, fractional.bytes / 4, phases.bytes / 4})
            throw std::runtime_error("Incomplete resident ROOM CUDA upload");
        sealed = true;
    }
};
using ResidentHandle = std::shared_ptr<ResidentBank>;
inline ResidentHandle resident(void* handle) {
    if (!handle || !*static_cast<ResidentHandle*>(handle))
        throw std::runtime_error("Invalid ROOM CUDA bank handle");
    auto value = *static_cast<ResidentHandle*>(handle);
    value->context.require();
    if (!value->sealed)
        throw std::runtime_error("ROOM CUDA bank is not sealed");
    return value;
}
