// Share only the established CUDA allocation/compiler helpers. The standalone
// probe entry point is renamed and never invoked by the Session backend.
#ifdef ROOM_GPU_RESIDENT
#include "resident_support.h"
#else
#define main room_gpu_standalone_unused_main
#include "probe.cpp"
#undef main
#endif
#include <memory>

#include "session_backend.h"
#include "session_layout.h"
#include "session_trace.h"

#ifdef ROOM_GPU_RESIDENT
#define room_gpu_session_destroy mradm_room_cuda_session_destroy
#define room_gpu_session_begin mradm_room_cuda_session_begin
#define room_gpu_session_voice mradm_room_cuda_session_voice
#define room_gpu_session_submit mradm_room_cuda_session_submit
#define room_gpu_session_finish mradm_room_cuda_session_finish
#define room_gpu_session_error mradm_room_cuda_session_error
#define room_gpu_late_destroy mradm_room_cuda_late_destroy
#define room_gpu_late_submit mradm_room_cuda_late_submit
#define room_gpu_late_finish mradm_room_cuda_late_finish
#endif

namespace {
#if (defined(ROOM_GPU_FUSE_TARGET) || defined(ROOM_GPU_FUSE_CONVOLVE)) && !defined(ROOM_GPU_RESIDENT)
#error FFT fusion requires the resident CUDA research adapter
#endif
#ifdef ROOM_GPU_FUSE_TARGET
constexpr bool fuse_target = true;
#else
constexpr bool fuse_target = false;
#endif
#ifdef ROOM_GPU_FUSE_CONVOLVE
constexpr bool fuse_convolve = true;
#else
constexpr bool fuse_convolve = false;
#endif
#if defined(ROOM_GPU_FUSE_TARGET) || defined(ROOM_GPU_FUSE_CONVOLVE)
struct FusedKernel {
    CUfunction function = nullptr;
    std::array<unsigned, 4> shape{}; // block x/y, shared bytes, FFTs per block
    void load(const Module& module, const char* name, const char* traits) {
        function = module.function(name);
        CUdeviceptr address = 0;
        size_t bytes = 0;
        driver(cuModuleGetGlobal(&address, &bytes, module.value, traits));
        if (bytes != sizeof(shape))
            throw std::runtime_error("Invalid fused FFT launch traits");
        driver(cuMemcpyDtoH(shape.data(), address, bytes));
        if (!shape[0] || !shape[1] || shape[0] * shape[1] > 1024 || shape[1] != shape[3] || shape[2] > 98304)
            throw std::runtime_error("Unsupported fused FFT launch shape");
        driver(
            cuFuncSetAttribute(function, CU_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES, static_cast<int>(shape[2])));
    }
    void launch(unsigned x, unsigned y, cudaStream_t stream, std::initializer_list<void*> values) const {
        std::array<void*, 12> arguments{};
        std::copy(values.begin(), values.end(), arguments.begin());
        driver(cuLaunchKernel(function,
                              x,
                              y,
                              1,
                              shape[0],
                              shape[1],
                              1,
                              shape[2],
                              reinterpret_cast<CUstream>(stream),
                              arguments.data(),
                              nullptr));
    }
};
#endif
constexpr unsigned max_voices = 32, max_updates = 256, max_ranges = 1024, max_tasks = 2048, max_jobs = 32 * 8192;
struct NativeUpdate {
    unsigned group, slot, parts, support, first_row;
    unsigned rows[64];
};
struct Info {
    unsigned active, reset, heads[2];
};
struct Header {
    unsigned voices, ranges, updates, tasks, fft_rows;
};
static_assert(sizeof(RoomGpuJob) == 264 && sizeof(RoomGpuTask) == 48 && sizeof(NativeUpdate) == 276);
#ifdef ROOM_GPU_RESIDENT
Config configuration(const ResidentHandle& resource, const RoomGpuParams& p) {
    Config c = resource->config;
#else
Config configuration(const fs::path& path, const RoomGpuParams& p) {
    Config c(path / "config.txt");
#endif
    if (p.rate != 96000 || p.fft != static_cast<unsigned>(c.n) || p.rows != static_cast<unsigned>(c.rows) ||
        p.taps == 0 || p.taps > 32768 || p.buckets == 0 || p.buckets > 64 || p.images == 0 || p.images > 1024 ||
        p.history_parts != (p.taps + 255) / 256)
        throw std::runtime_error("Unsupported GPU room shape/history");
    c.taps = p.taps;
    c.bucket_stride = p.bucket_stride;
    c.buckets = p.buckets;
    c.materials = p.images;
    return c;
}
// Graphs in one Engine execute serially on one stream and share identical
// input/output addresses. Reuse plans/workspaces across capacity variants.
struct Plans {
    struct Entry {
        int length = 0;
        cufftType type = CUFFT_R2C;
        unsigned rows = 0;
        std::shared_ptr<Plan> value;
    };
    cudaStream_t stream;
    std::vector<Entry> entries;
    explicit Plans(cudaStream_t s) : stream(s) {}
    std::shared_ptr<Plan> get(int length, cufftType type, unsigned rows) {
        const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
            return entry.length == length && entry.type == type && entry.rows == rows;
        });
        if (found != entries.end())
            return found->value;
        auto value = std::make_shared<Plan>(length, type, rows, stream);
        entries.push_back({length, type, rows, value});
        return value;
    }
};
#ifdef ROOM_GPU_COMPACT_UPLOAD
room_gpu_layout::Upload upload_layout(unsigned responses, unsigned updates, unsigned tasks) {
    return room_gpu_layout::Upload({sizeof(Header),
                                    (responses / 2) * sizeof(RoomGpuRange),
                                    updates * sizeof(NativeUpdate),
                                    tasks * sizeof(RoomGpuTask),
                                    max_voices * 2 * sizeof(RoomGpuRange),
                                    max_voices * sizeof(Info),
                                    max_voices * 4 * 257 * sizeof(float)});
}
#endif
struct Variant {
#ifdef ROOM_GPU_RESIDENT
    Context context;
#endif
    unsigned responses, updates, tasks, fft_rows;
#ifdef ROOM_GPU_COMPACT_UPLOAD
    room_gpu_layout::Upload upload;
#endif
    std::shared_ptr<Plan> inverse, target, output;
    cudaGraph_t graph = nullptr;
    cudaGraphExec_t execution = nullptr;
    Variant(unsigned r, unsigned u, unsigned t, unsigned f, const Config& c, Plans& plans)
        : responses(r), updates(u), tasks(t), fft_rows(f),
#ifdef ROOM_GPU_COMPACT_UPLOAD
          upload(upload_layout(r, u, t)),
#endif
          inverse(plans.get(c.n, CUFFT_C2R, r)), target(fuse_target ? nullptr : plans.get(512, CUFFT_R2C, f)),
          output(fuse_convolve ? nullptr : plans.get(512, CUFFT_C2R, t * 2)) {
    }
    ~Variant() {
#ifdef ROOM_GPU_RESIDENT
        if (!context.valid())
            return;
#endif
        if (execution)
            cudaGraphExecDestroy(execution);
        if (graph)
            cudaGraphDestroy(graph);
    }
};
struct Engine {
#ifdef ROOM_GPU_RESIDENT
    ResidentHandle resident_;
    room_cuda::Counted active_{room_cuda::engines};
#endif
    SessionTrace trace;
    Config c;
#ifdef ROOM_GPU_RESIDENT
    StreamOwner stream_owner;
    cudaStream_t& stream = stream_owner.value;
#else
    cudaStream_t stream = nullptr;
#endif
    std::unique_ptr<Module> module;
    size_t bins, parts, job_count = 0;
    Header header{};
    bool pending = false;
    Device bank, fractional, propagation, materials, phases, jobs, ranges, updates, tasks, groups, info, input,
        device_header;
    Device spectrum, bucket_time, packed, fresh, filters, history, convolved, convolved_time, output;
    Pinned host_jobs, host_ranges, host_updates, host_tasks, host_groups, host_info, host_input, host_header,
        host_output;
#ifdef ROOM_GPU_COMPACT_UPLOAD
    Device packet{upload_layout(2048, max_updates, max_tasks).bytes};
    Pinned host_packet{packet.bytes};
#endif
    std::vector<std::unique_ptr<Variant>> variants;
    CUfunction response_fn, pack_fn, install_fn, input_fn, convolve_fn, mix_fn;
#ifdef ROOM_GPU_FUSE_TARGET
    FusedKernel target_fused;
#endif
#ifdef ROOM_GPU_FUSE_CONVOLVE
    FusedKernel convolution_fused;
#endif
#ifdef ROOM_GPU_RESIDENT
    Engine(ResidentHandle resource, const RoomGpuParams& p)
        : resident_(std::move(resource)), c(configuration(resident_, p)), bins(c.n / 2 + 1),
          parts((c.taps + 255) / 256), bank(resident_->bank.data, resident_->bank.bytes),
          fractional(resident_->fractional.data, resident_->fractional.bytes), propagation(1025 * bins * 8),
          materials(c.materials * bins * 8), phases(resident_->phases.data, resident_->phases.bytes),
          jobs(max_jobs * sizeof(RoomGpuJob)), ranges(max_ranges * sizeof(RoomGpuRange)),
          updates(max_updates * sizeof(NativeUpdate)), tasks(max_tasks * sizeof(RoomGpuTask)),
          groups(max_voices * 2 * sizeof(RoomGpuRange)), info(max_voices * sizeof(Info)),
          input(max_voices * 2 * 257 * 8), device_header(sizeof(Header)), spectrum(2048 * bins * 8),
          bucket_time(2048 * c.n * 4), packed(fuse_target ? 1 : max_updates * 2 * parts * 512 * 4),
          fresh(fuse_target ? 1 : max_updates * 2 * parts * 257 * 8),
          filters(max_voices * 2 * 32 * 2 * parts * 257 * 8), history(max_voices * 2 * parts * 257 * 8),
          convolved(fuse_convolve ? 1 : max_tasks * 2 * 257 * 8), convolved_time(max_tasks * 2 * 512 * 4),
          output(max_voices * 4 * 256 * 4), host_jobs(jobs.bytes), host_ranges(ranges.bytes),
          host_updates(updates.bytes), host_tasks(tasks.bytes), host_groups(groups.bytes), host_info(info.bytes),
          host_input(input.bytes), host_header(sizeof(Header)), host_output(output.bytes) {
        initialize(p);
    }

#else
    Engine(const fs::path& fixture, const fs::path& source, const RoomGpuParams& p)
        : c(configuration(fixture, p)), bins(c.n / 2 + 1), parts((c.taps + 255) / 256), bank(c.rows * bins * 8),
          fractional(1025 * bins * 8), propagation(1025 * bins * 8), materials(c.materials * bins * 8), phases(c.n * 8),
          jobs(max_jobs * sizeof(RoomGpuJob)), ranges(max_ranges * sizeof(RoomGpuRange)),
          updates(max_updates * sizeof(NativeUpdate)), tasks(max_tasks * sizeof(RoomGpuTask)),
          groups(max_voices * 2 * sizeof(RoomGpuRange)), info(max_voices * sizeof(Info)),
          input(max_voices * 2 * 257 * 8), device_header(sizeof(Header)), spectrum(2048 * bins * 8),
          bucket_time(2048 * c.n * 4), packed(fuse_target ? 1 : max_updates * 2 * parts * 512 * 4),
          fresh(fuse_target ? 1 : max_updates * 2 * parts * 257 * 8),
          filters(max_voices * 2 * 32 * 2 * parts * 257 * 8), history(max_voices * 2 * parts * 257 * 8),
          convolved(fuse_convolve ? 1 : max_tasks * 2 * 257 * 8), convolved_time(max_tasks * 2 * 512 * 4),
          output(max_voices * 4 * 256 * 4), host_jobs(jobs.bytes), host_ranges(ranges.bytes),
          host_updates(updates.bytes), host_tasks(tasks.bytes), host_groups(groups.bytes), host_info(info.bytes),
          host_input(input.bytes), host_header(sizeof(Header)), host_output(output.bytes) {
        initialize(p, fixture, source);
    }
#endif
#ifdef ROOM_GPU_RESIDENT
    void initialize(const RoomGpuParams& p)
#else
    void initialize(const RoomGpuParams& p, const fs::path& fixture, const fs::path& source)
#endif
    {
        if (p.material_floats != materials.bytes / 4 || p.propagation_floats != propagation.bytes / 4)
            throw std::runtime_error("GPU table shape mismatch");
#ifndef ROOM_GPU_RESIDENT
        load(bank, fixture / "bank.bin");
        load(fractional, fixture / "fraction.bin");
        load(phases, fixture / "phase.bin");
#endif
        runtime(cudaMemcpy(materials.data, p.materials, materials.bytes, cudaMemcpyHostToDevice));
        runtime(cudaMemcpy(propagation.data, p.propagation, propagation.bytes, cudaMemcpyHostToDevice));
        runtime(cudaMemset(history.data, 0, history.bytes));
        runtime(cudaMemset(filters.data, 0, filters.bytes));
        // Active packed rows are fully overwritten. Inactive padded FFT rows
        // may retain earlier inputs; their outputs are never installed.
        runtime(cudaMemset(packed.data, 0, packed.bytes));
        runtime(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
        cudaDeviceProp props{};
        runtime(cudaGetDeviceProperties(&props, 0));
#ifdef ROOM_GPU_RESIDENT
        module = std::make_unique<Module>(resident_->source, c, 32, props.major, props.minor, true);
#else
        module = std::make_unique<Module>(source, c, 32, props.major, props.minor);
#endif
        response_fn = module->function("session_response");
        pack_fn = module->function("session_pack");
        install_fn = module->function("session_install");
        input_fn = module->function("session_input");
        convolve_fn = module->function("session_convolve");
        mix_fn = module->function("session_mix");
#ifdef ROOM_GPU_FUSE_TARGET
        target_fused.load(*module, "session_target_fused", "session_target_fft_traits");
#endif
#ifdef ROOM_GPU_FUSE_CONVOLVE
        convolution_fused.load(*module, "session_convolve_fused", "session_convolution_fft_traits");
#endif
        Plans plans(stream);
        for (const auto& shape : std::array<std::array<unsigned, 3>, 5>{
                 {{128, 32, 256}, {128, 64, 384}, {320, 96, 384}, {512, 128, 512}, {2048, 256, 2048}}}) {
            const unsigned maximum = shape[1] * 2 * static_cast<unsigned>(parts);
            for (unsigned rows = 128; rows < maximum; rows *= 2)
                variants.emplace_back(std::make_unique<Variant>(shape[0], shape[1], shape[2], rows, c, plans));
            variants.emplace_back(std::make_unique<Variant>(shape[0], shape[1], shape[2], maximum, c, plans));
        }
#ifdef ROOM_GPU_COMPACT_UPLOAD
        runtime(cudaMemset(packet.data, 0, packet.bytes));
        std::memset(host_packet.data, 0, host_packet.bytes);
#endif
        runtime(cudaMemset(device_header.data, 0, sizeof(Header)));
        for (auto& v : variants) {
            execute(*v);
            runtime(cudaStreamSynchronize(stream));
#ifdef ROOM_GPU_RESIDENT
            capture_graph(stream, [&] { execute(*v); }, v->graph, v->execution);
#else
            runtime(cudaStreamBeginCapture(stream, cudaStreamCaptureModeThreadLocal));
            execute(*v);
            runtime(cudaStreamEndCapture(stream, &v->graph));
            runtime(cudaGraphInstantiate(&v->execution, v->graph, 0));
#endif
        }
        std::memset(host_output.data, 0, host_output.bytes);
    }
    ~Engine() {
#ifdef ROOM_GPU_RESIDENT
        if (!resident_->context.valid()) {
            stream = nullptr;
            return;
        }
#endif
        cudaSetDevice(0);
        if (pending)
            cudaStreamSynchronize(stream);
        variants.clear();
        if (stream) {
            cudaStreamDestroy(stream);
            stream = nullptr;
        }
    }
    void launch(CUfunction f, unsigned x, unsigned y, std::initializer_list<void*> a) {
        std::array<void*, 12> args{};
        std::copy(a.begin(), a.end(), args.begin());
        driver(cuLaunchKernel(f, x, y, 1, 128, 1, 1, 0, reinterpret_cast<CUstream>(stream), args.data(), nullptr));
    }
    static unsigned blocks(size_t n) { return static_cast<unsigned>((n + 127) / 128); }
    void execute(const Variant& v) {
        auto address = [&](room_gpu_layout::Field field, void* separate) -> void* {
#ifdef ROOM_GPU_COMPACT_UPLOAD
            (void) separate;
            return static_cast<char*>(packet.data) + v.upload.offsets[field];
#else
            (void) field;
            return separate;
#endif
        };
        void* header_address = address(room_gpu_layout::header, device_header.data);
        void* ranges_address = address(room_gpu_layout::ranges, ranges.data);
        void* updates_address = address(room_gpu_layout::updates, updates.data);
        void* tasks_address = address(room_gpu_layout::tasks, tasks.data);
        void* groups_address = address(room_gpu_layout::groups, groups.data);
        void* info_address = address(room_gpu_layout::info, info.data);
        void* input_address = address(room_gpu_layout::input, input.data);
        trace.mark(1, stream);
        launch(response_fn,
#ifdef ROOM_GPU_FREQUENCY_MAJOR
               v.responses,
               blocks(bins),
#else
               blocks(bins),
               v.responses,
#endif
               {&header_address,
                &ranges_address,
                &jobs.data,
                &bank.data,
                &fractional.data,
                &propagation.data,
                &materials.data,
                &phases.data,
                &spectrum.data});
        trace.mark(2, stream);
        fft_check(cufftExecC2R(
            v.inverse->value, static_cast<cufftComplex*>(spectrum.data), static_cast<float*>(bucket_time.data)));
        trace.mark(3, stream);
#ifdef ROOM_GPU_FUSE_TARGET
        target_fused.launch((static_cast<unsigned>(parts) + target_fused.shape[3] - 1) / target_fused.shape[3],
                            v.updates * 2,
                            stream,
                            {&header_address, &updates_address, &bucket_time.data, &filters.data});
        trace.mark(4, stream);
        trace.mark(5, stream);
#else
        launch(pack_fn,
               blocks(parts * 512),
               v.updates * 2,
               {&header_address, &updates_address, &bucket_time.data, &packed.data});
        trace.mark(4, stream);
        fft_check(
            cufftExecR2C(v.target->value, static_cast<float*>(packed.data), static_cast<cufftComplex*>(fresh.data)));
        trace.mark(5, stream);
        launch(install_fn,
               blocks(2 * parts * 257),
               v.updates,
               {&header_address, &updates_address, &fresh.data, &filters.data});
#endif
        launch(input_fn, blocks(257), max_voices * 2, {&header_address, &info_address, &input_address, &history.data});
        trace.mark(6, stream);
#ifdef ROOM_GPU_FUSE_CONVOLVE
        convolution_fused.launch(
            (v.tasks * 2 + convolution_fused.shape[3] - 1) / convolution_fused.shape[3],
            1,
            stream,
            {&header_address, &info_address, &tasks_address, &history.data, &filters.data, &convolved_time.data});
        trace.mark(7, stream);
        trace.mark(8, stream);
#else
        launch(convolve_fn,
               blocks(257),
               v.tasks * 2,
               {&header_address, &info_address, &tasks_address, &history.data, &filters.data, &convolved.data});
        trace.mark(7, stream);
        fft_check(cufftExecC2R(
            v.output->value, static_cast<cufftComplex*>(convolved.data), static_cast<float*>(convolved_time.data)));
        trace.mark(8, stream);
#endif
        unsigned fade = c.rate / 100;
        launch(mix_fn,
               blocks(512),
               max_voices * 2,
               {&header_address, &groups_address, &tasks_address, &convolved_time.data, &output.data, &fade});
        trace.mark(9, stream);
        runtime(cudaMemcpyAsync(host_output.data, output.data, output.bytes, cudaMemcpyDeviceToHost, stream));
        trace.mark(10, stream);
    }
    void begin(unsigned voices) {
#ifdef ROOM_GPU_RESIDENT
        resident_->context.require();
#endif
        trace.begin();
        if (pending || voices > max_voices)
            throw std::runtime_error("Invalid GPU batch lifecycle/capacity");
        runtime(cudaSetDevice(0));
        header = {voices, 0, 0, 0, 0};
        job_count = 0;
        std::memset(host_info.data, 0, host_info.bytes);
        std::memset(host_groups.data, 0, host_groups.bytes);
    }
    void add(unsigned voice, const RoomGpuFrame& f) {
        if (voice >= header.voices || job_count + f.job_count > max_jobs ||
            header.updates + f.update_count > max_updates || header.tasks + f.task_count > max_tasks ||
            f.heads[0] >= parts || f.heads[1] >= parts)
            throw std::runtime_error("GPU frame capacity/shape exceeded");
        auto* dst_jobs = static_cast<RoomGpuJob*>(host_jobs.data);
        if (f.job_count)
            std::memcpy(dst_jobs + job_count, f.jobs, f.job_count * sizeof(RoomGpuJob));
        static_cast<Info*>(host_info.data)[voice] = {f.active, f.reset, {f.heads[0], f.heads[1]}};
        std::memcpy(static_cast<float*>(host_input.data) + voice * 4 * 257, f.frequency, 4 * 257 * sizeof(float));
        for (size_t i = 0; i < f.update_count; ++i) {
            auto u = f.updates[i];
            if (u.slot >= 32 || u.branch >= 2 || u.range_count != static_cast<unsigned>(c.buckets) ||
                u.range_first + u.range_count > f.range_count || u.parts > parts || !u.support ||
                u.support > static_cast<unsigned>(c.taps) || u.parts != (u.support + 255) / 256)
                throw std::runtime_error("Invalid GPU target");
            NativeUpdate update{};
            update.group = voice * 2 + u.branch;
            update.slot = u.slot;
            update.parts = u.parts;
            update.support = u.support;
            update.first_row = header.fft_rows;
            header.fft_rows += u.parts * 2;
            std::fill(std::begin(update.rows), std::end(update.rows), 0xffffffffu);
            for (unsigned b = 0; b < u.range_count; ++b) {
                auto range = f.ranges[u.range_first + b];
                if (!range.count)
                    continue;
                if (header.ranges == max_ranges || range.first + range.count > f.job_count)
                    throw std::runtime_error("GPU response bucket capacity exceeded");
                update.rows[b] = header.ranges;
                range.first += static_cast<unsigned>(job_count);
                static_cast<RoomGpuRange*>(host_ranges.data)[header.ranges++] = range;
            }
            static_cast<NativeUpdate*>(host_updates.data)[header.updates++] = update;
        }
        for (unsigned branch = 0; branch < 2; ++branch) {
            RoomGpuRange group{header.tasks, 0};
            for (size_t j = 0; j < f.task_count; ++j) {
                auto task = f.tasks[j];
                if (task.branch != branch)
                    continue;
                if (task.slot >= 32 || task.parts > parts || task.begin + task.frames > 256)
                    throw std::runtime_error("Invalid GPU render task");
                task.branch = voice * 2 + branch;
                static_cast<RoomGpuTask*>(host_tasks.data)[header.tasks++] = task;
                ++group.count;
            }
            static_cast<RoomGpuRange*>(host_groups.data)[voice * 2 + branch] = group;
        }
        job_count += f.job_count;
    }
    void submit() {
        const auto found = std::find_if(variants.begin(), variants.end(), [&](const auto& v) {
            return header.ranges * 2 <= v->responses && header.updates <= v->updates && header.tasks <= v->tasks &&
                   header.fft_rows <= v->fft_rows;
        });
        if (found == variants.end())
            throw std::runtime_error("GPU graph shape capacity exceeded");
        const auto* selected = found->get();
        trace.submit(
            header.ranges * 2, header.updates, header.tasks, selected->responses, selected->updates, selected->tasks);
        trace.mark(0, stream);
#ifdef ROOM_GPU_COMPACT_UPLOAD
        auto pack = [&](room_gpu_layout::Field field, const void* from, size_t bytes) {
            if (bytes > selected->upload.capacities[field])
                throw std::runtime_error("GPU compact upload capacity exceeded");
            if (bytes)
                std::memcpy(static_cast<char*>(host_packet.data) + selected->upload.offsets[field], from, bytes);
        };
        pack(room_gpu_layout::header, &header, sizeof(header));
        pack(room_gpu_layout::ranges, host_ranges.data, header.ranges * sizeof(RoomGpuRange));
        pack(room_gpu_layout::updates, host_updates.data, header.updates * sizeof(NativeUpdate));
        pack(room_gpu_layout::tasks, host_tasks.data, header.tasks * sizeof(RoomGpuTask));
        pack(room_gpu_layout::groups, host_groups.data, header.voices * 2 * sizeof(RoomGpuRange));
        pack(room_gpu_layout::info, host_info.data, header.voices * sizeof(Info));
        pack(room_gpu_layout::input, host_input.data, header.voices * 4 * 257 * sizeof(float));
        if (job_count)
            runtime(cudaMemcpyAsync(
                jobs.data, host_jobs.data, job_count * sizeof(RoomGpuJob), cudaMemcpyHostToDevice, stream));
        runtime(cudaMemcpyAsync(packet.data, host_packet.data, selected->upload.bytes, cudaMemcpyHostToDevice, stream));
#else
        std::memcpy(host_header.data, &header, sizeof(header));
        auto copy = [&](Device& to, Pinned& from, size_t size) {
            if (size)
                runtime(cudaMemcpyAsync(to.data, from.data, size, cudaMemcpyHostToDevice, stream));
        };
        copy(jobs, host_jobs, job_count * sizeof(RoomGpuJob));
        copy(ranges, host_ranges, header.ranges * sizeof(RoomGpuRange));
        copy(updates, host_updates, header.updates * sizeof(NativeUpdate));
        copy(tasks, host_tasks, header.tasks * sizeof(RoomGpuTask));
        copy(groups, host_groups, header.voices * 2 * sizeof(RoomGpuRange));
        copy(info, host_info, header.voices * sizeof(Info));
        copy(input, host_input, header.voices * 4 * 257 * sizeof(float));
        copy(device_header, host_header, sizeof(Header));
#endif
        runtime(cudaGraphLaunch(selected->execution, stream));
        trace.launched();
        pending = true;
    }
    void finish(float* destination, size_t samples) {
#ifdef ROOM_GPU_RESIDENT
        resident_->context.require();
#endif
        if (!pending || samples != header.voices * 4 * 256)
            throw std::runtime_error("Invalid GPU completion");
        trace.wait();
        runtime(cudaStreamSynchronize(stream));
        trace.finish();
        pending = false;
        if (samples)
            std::memcpy(destination, host_output.data, samples * sizeof(float));
    }
}; // namespace
#include "session_late.h"
thread_local std::array<char, 1024> last_error{};
void remember(const char* message) noexcept {
    const size_t size = std::min(std::strlen(message), last_error.size() - 1);
    std::memcpy(last_error.data(), message, size);
    last_error[size] = 0;
}
} // namespace
#define GPU_CATCH(result)                                                                                              \
    catch (const std::exception& e) {                                                                                  \
        remember(e.what());                                                                                            \
        return result;                                                                                                 \
    }                                                                                                                  \
    catch (...) {                                                                                                      \
        remember("Unknown CUDA failure");                                                                              \
        return result;                                                                                                 \
    }
#ifdef ROOM_GPU_RESIDENT
extern "C"
    __declspec(dllexport) void* mradm_room_cuda_session_create(void* bank, const RoomGpuParams* params) noexcept {
    try {
        if (!params)
            throw std::runtime_error("Missing ROOM CUDA parameters");
        runtime(cudaSetDevice(0));
        return new Engine(resident(bank), *params);
    }
    GPU_CATCH(nullptr)
}
#else
extern "C" __declspec(dllexport) void*
room_gpu_session_create(const char* fixture, const char* kernels, const RoomGpuParams* params) noexcept {
    try {
        runtime(cudaSetDevice(0));
        runtime(cudaFree(nullptr));
        return new Engine(fs::u8path(fixture), fs::u8path(kernels), *params);
    }
    GPU_CATCH(nullptr)
}
#endif
extern "C" __declspec(dllexport) void room_gpu_session_destroy(void* p) noexcept {
    try {
        delete static_cast<Engine*>(p);
    }
    GPU_CATCH(void())
}
extern "C" __declspec(dllexport) int room_gpu_session_begin(void* p, unsigned count) noexcept {
    try {
        static_cast<Engine*>(p)->begin(count);
        return 0;
    }
    GPU_CATCH(1)
}
extern "C" __declspec(dllexport) int room_gpu_session_voice(void* p, unsigned index, const RoomGpuFrame* f) noexcept {
    try {
        static_cast<Engine*>(p)->add(index, *f);
        return 0;
    }
    GPU_CATCH(1)
}
extern "C" __declspec(dllexport) int room_gpu_session_submit(void* p) noexcept {
    try {
        static_cast<Engine*>(p)->submit();
        return 0;
    }
    GPU_CATCH(1)
}
extern "C" __declspec(dllexport) int room_gpu_session_finish(void* p, float* out, size_t samples) noexcept {
    try {
        static_cast<Engine*>(p)->finish(out, samples);
        return 0;
    }
    GPU_CATCH(1)
}
extern "C" __declspec(dllexport) const char* room_gpu_session_error() noexcept {
    return last_error.data();
}
#ifdef ROOM_GPU_RESIDENT
extern "C" __declspec(dllexport) void* mradm_room_cuda_late_create(
    void* bank, unsigned parts, unsigned channels, const float* filters, size_t floats) noexcept {
    try {
        runtime(cudaSetDevice(0));
        if (!filters || !parts || parts > 512 || !channels || channels > 64 ||
            floats != size_t(parts) * channels * 2049 * 2)
            throw std::runtime_error("Unsupported GPU late shape");
        return new LateEngine(resident(bank), parts, channels, filters);
    }
    GPU_CATCH(nullptr)
}
#else
extern "C" __declspec(dllexport) void* room_gpu_late_create(const char* fixture,
                                                            const char* kernels,
                                                            unsigned parts,
                                                            unsigned channels,
                                                            const float* filters,
                                                            size_t floats) noexcept {
    try {
        runtime(cudaSetDevice(0));
        if (!parts || parts > 512 || !channels || channels > 64 || floats != size_t(parts) * channels * 2049 * 2)
            throw std::runtime_error("Unsupported GPU late shape");
        return new LateEngine(fs::u8path(fixture), fs::u8path(kernels), parts, channels, filters);
    }
    GPU_CATCH(nullptr)
}
#endif
extern "C" __declspec(dllexport) void room_gpu_late_destroy(void* p) noexcept {
    try {
        delete static_cast<LateEngine*>(p);
    }
    GPU_CATCH(void())
}
extern "C" __declspec(dllexport) int
room_gpu_late_submit(void* p, const float* row, size_t floats, unsigned reset) noexcept {
    try {
        static_cast<LateEngine*>(p)->submit(row, floats, reset != 0);
        return 0;
    }
    GPU_CATCH(1)
}
extern "C" __declspec(dllexport) int room_gpu_late_finish(void* p, float* out, size_t samples) noexcept {
    try {
        static_cast<LateEngine*>(p)->finish(out, samples);
        return 0;
    }
    GPU_CATCH(1)
}
#undef GPU_CATCH
