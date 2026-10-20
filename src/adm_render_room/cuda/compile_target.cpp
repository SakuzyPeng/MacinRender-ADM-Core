// Build-time compiler for the fixed 512-point target FFT. SDK headers stay on
// the build host; the application ships only the generated device program.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nvrtc.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(nvrtcResult result) {
    if (result != NVRTC_SUCCESS)
        throw std::runtime_error(nvrtcGetErrorString(result));
}
std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    nvrtcProgram program = nullptr;
    try {
        if (argc != 6)
            throw std::runtime_error("Expected source, output, SM architecture, MathDx include and CUDA include");
        const std::filesystem::path source_path(argv[1]), output_path(argv[2]);
        const auto architecture = std::stoi(argv[3]);
        std::ifstream input(source_path, std::ios::binary);
        if (!input)
            throw std::runtime_error("Cannot read target FFT source");
        const std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        check(nvrtcCreateProgram(&program, source.c_str(), "room_target_fft.cu", 0, nullptr, nullptr));
        const std::vector<std::string> options = {"--std=c++17",
                                                  "--fmad=true",
                                                  "--ftz=false",
                                                  "--prec-div=true",
                                                  "--prec-sqrt=true",
                                                  "--gpu-architecture=compute_" + std::to_string(architecture),
                                                  "--device-as-default-execution-space",
                                                  "-DCUFFTDX_DISABLE_CUTLASS_DEPENDENCY",
                                                  "--include-path=" + utf8(std::filesystem::path(argv[4])),
                                                  "--include-path=" + utf8(std::filesystem::path(argv[5])),
                                                  "-DFFT_N=4096",
                                                  "-DTARGETS=32",
                                                  "-DROOM_GPU_FFT_LIBRARY",
                                                  "-DROOM_GPU_FUSE_TARGET",
                                                  "-DROOM_FFT_SM=" + std::to_string(architecture * 10)};
        std::vector<const char*> pointers;
        for (const auto& value : options)
            pointers.push_back(value.c_str());
        const auto compiled = nvrtcCompileProgram(program, static_cast<int>(pointers.size()), pointers.data());
        if (compiled != NVRTC_SUCCESS) {
            size_t size = 0;
            check(nvrtcGetProgramLogSize(program, &size));
            std::vector<char> log(size + 1);
            check(nvrtcGetProgramLog(program, log.data()));
            throw std::runtime_error(log.data());
        }
        size_t size = 0;
        check(nvrtcGetPTXSize(program, &size));
        std::vector<char> ptx(size);
        check(nvrtcGetPTX(program, ptx.data()));
        check(nvrtcDestroyProgram(&program));
        std::filesystem::create_directories(output_path.parent_path());
        std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
        output.write(ptx.data(), static_cast<std::streamsize>(ptx.size()));
        output.close();
        if (!output)
            throw std::runtime_error("Cannot write target FFT device program");
        return 0;
    } catch (const std::exception& error) {
        if (program)
            nvrtcDestroyProgram(&program);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
