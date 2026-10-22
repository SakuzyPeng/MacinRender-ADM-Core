#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off
#include <windows.h>
#include <evntprov.h>
#include <tlhelp32.h>
// clang-format on

#include <array>
#include <atomic>
#include <cuda_profiler_api.h>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <nvtx3/nvToolsExt.h>
namespace {
std::atomic<unsigned long long> current_call{0};
std::atomic<unsigned> mode{0};
constexpr std::size_t capacity = 2097152;
struct Record {
    unsigned long long ticks, call;
    unsigned tid, stage, begin, reserved;
};
static_assert(sizeof(Record) == 32);
Record records[capacity]{};
std::atomic<std::size_t> count{0};
LARGE_INTEGER frequency{}, start_before{}, start_after{}, end_before{}, end_after{};
std::array<unsigned, 32> workers{};
unsigned worker_count = 0;
REGHANDLE provider = 0;
const GUID provider_id = {0x973c1dc0, 0x11c4, 0x4ae9, {0x8f, 0x78, 0x31, 0x6c, 0x8d, 0x58, 0x6f, 0x8a}};
void sync_event(unsigned short id, LARGE_INTEGER& before, LARGE_INTEGER& after) {
    EVENT_DESCRIPTOR desc{};
    desc.Id = id;
    desc.Level = 4;
    desc.Keyword = 1;
    QueryPerformanceCounter(&before);
    EventWrite(provider, &desc, 0, nullptr);
    QueryPerformanceCounter(&after);
}
void record(unsigned stage, unsigned begin, unsigned long long call) {
    LARGE_INTEGER tick;
    QueryPerformanceCounter(&tick);
    const auto at = count.fetch_add(1, std::memory_order_relaxed);
    if (at < capacity)
        records[at] = {static_cast<unsigned long long>(tick.QuadPart), call, GetCurrentThreadId(), stage, begin, 0};
}
void push(const char* name, unsigned long long call) {
    nvtxEventAttributes_t event{};
    event.version = NVTX_VERSION;
    event.size = NVTX_EVENT_ATTRIB_STRUCT_SIZE;
    event.messageType = NVTX_MESSAGE_TYPE_ASCII;
    event.message.ascii = name;
    event.payloadType = NVTX_PAYLOAD_TYPE_UNSIGNED_INT64;
    event.payload.ullValue = call;
    nvtxRangePushEx(&event);
}
void find_workers() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry))
        do {
            if (entry.th32OwnerProcessID != GetCurrentProcessId())
                continue;
            HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
            if (!thread)
                continue;
            PWSTR name = nullptr;
            if (SUCCEEDED(GetThreadDescription(thread, &name))) {
                if (name && std::wstring_view(name).starts_with(L"mradm-room-") && worker_count < workers.size())
                    workers[worker_count++] = entry.th32ThreadID;
                LocalFree(name);
            }
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    CloseHandle(snapshot);
}
} // namespace
extern "C" __declspec(dllexport) void room_timeline_event(unsigned stage, unsigned begin) {
    static const char* names[] = {"session_process",
                                  "canonical_block",
                                  "hrir_query",
                                  "source_response",
                                  "target_filters",
                                  "emitter_begin",
                                  "emitter_convolution",
                                  "worker_roundtrip",
                                  "worker_execution",
                                  "source_reduction",
                                  "late_field",
                                  "late_decoder",
                                  "voice_render",
                                  "voice_batch",
                                  "voice_segment",
                                  "path_transfers",
                                  "response_transform",
                                  "gpu_submit",
                                  "gpu_wait"};
    if (stage >= 19)
        return;
    unsigned selected = mode.load(std::memory_order_relaxed);
    if (!selected)
        return;
    auto call = current_call.load(std::memory_order_relaxed);
    if (selected == 1)
        record(stage, begin, call);
    else if (begin)
        push(names[stage], call);
    else
        nvtxRangePop();
}
extern "C" __declspec(dllexport) void room_timeline_call(unsigned long long call, unsigned begin) {
    if (begin)
        current_call.store(call, std::memory_order_relaxed);
    if (mode.load(std::memory_order_relaxed) == 1)
        record(19, begin, call);
    else if (begin)
        push("benchmark_call", call);
    else
        nvtxRangePop();
}
extern "C" __declspec(dllexport) int room_timeline_capture() {
    if (_wgetenv(L"MR_ROOM_FLIGHT_LOG")) {
        // Commit recorder pages and discover worker IDs before the 32 warmup calls.
        auto* pages = reinterpret_cast<volatile Record*>(records);
        for (std::size_t i = 0; i < capacity; i += 128)
            pages[i].ticks = 0;
        QueryPerformanceFrequency(&frequency);
        find_workers();
        const auto error = EventRegister(&provider_id, nullptr, nullptr, &provider);
        if (error)
            return static_cast<int>(error);
        sync_event(1, start_before, start_after);
        mode.store(1, std::memory_order_relaxed);
        return 0;
    }
    mode.store(2, std::memory_order_relaxed);
    return static_cast<int>(cudaProfilerStart());
}
extern "C" __declspec(dllexport) int room_timeline_flush() {
    if (mode.load(std::memory_order_relaxed) != 1)
        return 0;
    sync_event(2, end_before, end_after);
    mode.store(0, std::memory_order_relaxed);
    EventUnregister(provider);
    const auto size = count.load(std::memory_order_relaxed);
    if (size > capacity)
        return 1;
    try {
        const auto* name = _wgetenv(L"MR_ROOM_FLIGHT_LOG");
        if (!name)
            return 2;
        std::ofstream out(std::filesystem::path(name), std::ios::binary);
        out << "{\"schema\":\"mradm.room-flight\",\"version\":1,\"frequency\":" << frequency.QuadPart
            << ",\"records\":" << size << ",\"pid\":" << GetCurrentProcessId()
            << ",\"main_tid\":" << GetCurrentThreadId() << ",\"start_before\":" << start_before.QuadPart
            << ",\"start_after\":" << start_after.QuadPart << ",\"end_before\":" << end_before.QuadPart
            << ",\"end_after\":" << end_after.QuadPart << ",\"workers\":[";
        for (unsigned i = 0; i < worker_count; ++i) {
            if (i)
                out << ',';
            out << workers[i];
        }
        out << "]}\n";
        out.write(reinterpret_cast<const char*>(records), static_cast<std::streamsize>(size * sizeof(Record)));
        return out ? 0 : 3;
    } catch (...) {
        return 4;
    }
}
