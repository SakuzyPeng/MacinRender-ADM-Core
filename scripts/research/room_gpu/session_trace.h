// Diagnostic builds only. All event/record storage is prepared before audio;
// records are emitted on destruction, after the benchmark has finished.
#pragma once
#ifdef ROOM_GPU_TRACE
struct SessionTrace {
    struct Record {
        unsigned rows = 0, updates = 0, tasks = 0, padded_rows = 0, padded_updates = 0, padded_tasks = 0;
        double collect_ms = 0, submit_ms = 0, overlap_ms = 0, wait_ms = 0;
        std::array<float, 10> device_ms{};
    };
    std::array<cudaEvent_t, 11> events{};
    std::vector<Record> records = std::vector<Record>(4096);
    size_t count = 0;
    Record record;
    Clock::time_point started, submitting, submitted, waiting;
    SessionTrace() {
        for (auto& event : events)
            runtime(cudaEventCreate(&event));
    }
    ~SessionTrace() {
        for (size_t i = 0; i < count; ++i) {
            const auto& r = records[i];
            std::cerr << "GPU_TRACE " << i << ' ' << r.rows << ' ' << r.updates << ' ' << r.tasks << ' '
                      << r.padded_rows << ' ' << r.padded_updates << ' ' << r.padded_tasks << ' ' << r.collect_ms << ' '
                      << r.submit_ms << ' ' << r.overlap_ms << ' ' << r.wait_ms;
            for (auto ms : r.device_ms)
                std::cerr << ' ' << ms;
            std::cerr << '\n';
        }
        for (auto event : events)
            cudaEventDestroy(event);
    }
    void mark(unsigned index, cudaStream_t stream) {
        cudaStreamCaptureStatus status;
        runtime(cudaStreamIsCapturing(stream, &status));
        runtime(cudaEventRecordWithFlags(
            events[index], stream, status == cudaStreamCaptureStatusActive ? cudaEventRecordExternal : 0));
    }
    void begin() { started = Clock::now(); }
    void submit(unsigned rows, unsigned updates, unsigned tasks, unsigned pr, unsigned pu, unsigned pt) {
        submitting = Clock::now();
        record = {};
        record.rows = rows;
        record.updates = updates;
        record.tasks = tasks;
        record.padded_rows = pr;
        record.padded_updates = pu;
        record.padded_tasks = pt;
        record.collect_ms = std::chrono::duration<double, std::milli>(submitting - started).count();
    }
    void launched() {
        submitted = Clock::now();
        record.submit_ms = std::chrono::duration<double, std::milli>(submitted - submitting).count();
    }
    void wait() {
        waiting = Clock::now();
        record.overlap_ms = std::chrono::duration<double, std::milli>(waiting - submitted).count();
    }
    void finish() {
        record.wait_ms = std::chrono::duration<double, std::milli>(Clock::now() - waiting).count();
        for (unsigned i = 0; i < record.device_ms.size(); ++i)
            runtime(cudaEventElapsedTime(&record.device_ms[i], events[i], events[i + 1]));
        if (count < records.size())
            records[count++] = record;
    }
};
#else
struct SessionTrace {
    static void mark(unsigned, cudaStream_t) {}
    static void begin() {}
    static void submit(unsigned, unsigned, unsigned, unsigned, unsigned, unsigned) {}
    static void launched() {}
    static void wait() {}
    static void finish() {}
};
#endif
