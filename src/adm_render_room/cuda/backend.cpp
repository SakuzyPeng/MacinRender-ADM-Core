// The validated processing graph is shared with the research adapter. This
// build selects resident prepared resources and exposes no fixture/file API.
#define ROOM_GPU_RESIDENT 1
#include "../../../scripts/research/room_gpu/session_backend.cpp"
#define ROOM_CUDA_CATCH(result)                                                                                        \
    catch (const std::exception& e) {                                                                                  \
        remember(e.what());                                                                                            \
        return result;                                                                                                 \
    }                                                                                                                  \
    catch (...) {                                                                                                      \
        remember("Unknown CUDA resource failure");                                                                     \
        return result;                                                                                                 \
    }


extern "C" __declspec(dllexport) unsigned mradm_room_cuda_optimizations() noexcept {
    unsigned flags = 0;
#ifdef ROOM_GPU_COMPACT_UPLOAD
    flags |= 2;
#endif
#ifdef ROOM_GPU_FREQUENCY_MAJOR
    flags |= 16;
#endif
#ifdef ROOM_GPU_FUSE_TARGET
    flags |= 4;
#endif
#ifdef ROOM_GPU_FUSE_CONVOLVE
    flags |= 8;
#endif
    return flags;
}
extern "C" __declspec(dllexport) unsigned mradm_room_cuda_abi() noexcept {
    return 1;
}
extern "C" __declspec(dllexport) void*
mradm_room_cuda_bank_create(const RoomCudaBankDesc* desc, const char* source, size_t length) noexcept {
    try {
        if (!desc || !source || !length || length > 1024 * 1024)
            throw std::runtime_error("Invalid ROOM CUDA source/descriptor");
        runtime(cudaSetDevice(0));
        runtime(cudaFree(nullptr));
        return new ResidentHandle(std::make_shared<ResidentBank>(*desc, source, length));
    }
    ROOM_CUDA_CATCH(nullptr)
}
extern "C" __declspec(dllexport) int
mradm_room_cuda_bank_upload(void* bank, unsigned table, size_t offset, const float* data, size_t count) noexcept {
    try {
        if (!bank || !*static_cast<ResidentHandle*>(bank))
            throw std::runtime_error("Invalid ROOM CUDA upload handle");
        (*static_cast<ResidentHandle*>(bank))->upload(table, offset, data, count);
        return 0;
    }
    ROOM_CUDA_CATCH(1)
}
extern "C" __declspec(dllexport) int mradm_room_cuda_bank_seal(void* bank) noexcept {
    try {
        if (!bank || !*static_cast<ResidentHandle*>(bank))
            throw std::runtime_error("Invalid ROOM CUDA seal handle");
        (*static_cast<ResidentHandle*>(bank))->seal();
        return 0;
    }
    ROOM_CUDA_CATCH(1)
}
extern "C" __declspec(dllexport) void mradm_room_cuda_bank_destroy(void* bank) noexcept {
    try {
        cudaSetDevice(0);
        delete static_cast<ResidentHandle*>(bank);
    }
    ROOM_CUDA_CATCH(void())
}
extern "C" __declspec(dllexport) const char* mradm_room_cuda_bank_device(void* bank) noexcept {
    try {
        return resident(bank)->properties.name;
    }
    ROOM_CUDA_CATCH(nullptr)
}
extern "C" __declspec(dllexport) int mradm_room_cuda_stats(RoomCudaStats* out) noexcept {
    if (!out)
        return 1;
    *out = {room_cuda::banks.load(),
            room_cuda::engines.load(),
            room_cuda::late_engines.load(),
            room_cuda::device_bytes.load(),
            room_cuda::pinned_bytes.load()};
    return 0;
}
