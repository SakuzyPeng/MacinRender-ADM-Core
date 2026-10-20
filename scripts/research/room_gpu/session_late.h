// Included inside the private Session backend namespace.
// The long partition result is consumed by the next canonical block, preserving
// the original 2048-frame tail offset. A separate stream overlaps current audio.
struct LateStream {
#ifdef ROOM_GPU_RESIDENT
    Context context;
#endif
    cudaStream_t value = nullptr;
    LateStream() { runtime(cudaStreamCreateWithFlags(&value, cudaStreamNonBlocking)); }
    ~LateStream() {
#ifdef ROOM_GPU_RESIDENT
        if (!context.valid())
            return;
#endif
        cudaStreamDestroy(value);
    }
};
struct LateGraph {
#ifdef ROOM_GPU_RESIDENT
    Context context;
#endif
    cudaGraph_t graph = nullptr;
    cudaGraphExec_t execution = nullptr;
    ~LateGraph() {
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
cudaDeviceProp late_device_properties() {
    cudaDeviceProp properties{};
    runtime(cudaGetDeviceProperties(&properties, 0));
    return properties;
}
struct LateEngine {
#ifdef ROOM_GPU_RESIDENT
    ResidentHandle resident_;
    room_cuda::Counted active_{room_cuda::late_engines};
#endif
    unsigned parts, channels, head;
    bool pending = false;
    LateStream stream;
    Device filters, history, input, parameters, spectrum, time, output;
    Pinned host_input, host_parameters, host_output;
    cudaDeviceProp properties = late_device_properties();
    Module module;
    Plan inverse;
    LateGraph graph;
    CUfunction input_fn, convolve_fn, extract_fn;
#ifdef ROOM_GPU_RESIDENT
    LateEngine(ResidentHandle resource, unsigned p, unsigned c, const float* f)
        : resident_(std::move(resource)), parts(p), channels(c), head(p - 1), filters(size_t(c) * p * 2049 * 8),
          history(size_t(p) * 2049 * 8), input(2049 * 8), parameters(8), spectrum(c * 2049 * 8), time(c * 4096 * 4),
          output(c * 2048 * 4), host_input(input.bytes), host_parameters(parameters.bytes), host_output(output.bytes),
          module(resident_->source, resident_->config, 32, properties.major, properties.minor),
          inverse(4096, CUFFT_C2R, c, stream.value) {
        initialize(f);
    }
#else
    LateEngine(const fs::path& fixture, const fs::path& source, unsigned p, unsigned c, const float* f)
        : parts(p), channels(c), head(p - 1), filters(size_t(c) * p * 2049 * 8), history(size_t(p) * 2049 * 8),
          input(2049 * 8), parameters(8), spectrum(c * 2049 * 8), time(c * 4096 * 4), output(c * 2048 * 4),
          host_input(input.bytes), host_parameters(parameters.bytes), host_output(output.bytes),
          module(source, Config(fixture / "config.txt"), 32, properties.major, properties.minor),
          inverse(4096, CUFFT_C2R, c, stream.value) {
        initialize(f);
    }
#endif
    void initialize(const float* f) {
        runtime(cudaMemcpy(filters.data, f, filters.bytes, cudaMemcpyHostToDevice));
        runtime(cudaMemset(history.data, 0, history.bytes));
        std::memset(host_input.data, 0, host_input.bytes);
        std::memset(host_parameters.data, 0, host_parameters.bytes);
        input_fn = module.function("late_input");
        convolve_fn = module.function("late_convolve");
        extract_fn = module.function("late_extract");
        execute();
        runtime(cudaStreamSynchronize(stream.value));
#ifdef ROOM_GPU_RESIDENT
        capture_graph(stream.value, [&] { execute(); }, graph.graph, graph.execution);
#else
        runtime(cudaStreamBeginCapture(stream.value, cudaStreamCaptureModeThreadLocal));
        execute();
        runtime(cudaStreamEndCapture(stream.value, &graph.graph));
        runtime(cudaGraphInstantiate(&graph.execution, graph.graph, 0));
#endif
    }
    ~LateEngine() {
#ifdef ROOM_GPU_RESIDENT
        if (!resident_->context.valid())
            return;
#endif
        cudaSetDevice(0);
        if (pending)
            cudaStreamSynchronize(stream.value);
    }
    void execute() {
        runtime(cudaMemcpyAsync(input.data, host_input.data, input.bytes, cudaMemcpyHostToDevice, stream.value));
        runtime(cudaMemcpyAsync(
            parameters.data, host_parameters.data, parameters.bytes, cudaMemcpyHostToDevice, stream.value));
        auto launch = [&](CUfunction fn, unsigned x, unsigned y, std::initializer_list<void*> values) {
            std::array<void*, 8> args{};
            std::copy(values.begin(), values.end(), args.begin());
            driver(cuLaunchKernel(
                fn, x, y, 1, 128, 1, 1, 0, reinterpret_cast<CUstream>(stream.value), args.data(), nullptr));
        };
        launch(input_fn, 17, 1, {&parameters.data, &input.data, &history.data, &parts});
        launch(convolve_fn, 17, channels, {&parameters.data, &history.data, &filters.data, &spectrum.data, &parts});
        fft_check(
            cufftExecC2R(inverse.value, static_cast<cufftComplex*>(spectrum.data), static_cast<float*>(time.data)));
        launch(extract_fn, 16, channels, {&time.data, &output.data});
        runtime(cudaMemcpyAsync(host_output.data, output.data, output.bytes, cudaMemcpyDeviceToHost, stream.value));
    }
    void submit(const float* row, size_t floats, bool reset) {
#ifdef ROOM_GPU_RESIDENT
        resident_->context.require();
#endif
        if (pending || floats != 4098)
            throw std::runtime_error("Invalid GPU late submission");
        runtime(cudaSetDevice(0));
        head = reset ? 0 : (head + 1) % parts;
        std::memcpy(host_input.data, row, input.bytes);
        auto* p = static_cast<unsigned*>(host_parameters.data);
        p[0] = head;
        p[1] = reset;
        runtime(cudaGraphLaunch(graph.execution, stream.value));
        pending = true;
    }
    void finish(float* out, size_t samples) {
#ifdef ROOM_GPU_RESIDENT
        resident_->context.require();
#endif
        if (!pending || samples * sizeof(float) != output.bytes)
            throw std::runtime_error("Invalid GPU late completion");
        runtime(cudaSetDevice(0));
        runtime(cudaStreamSynchronize(stream.value));
        pending = false;
        std::memcpy(out, host_output.data, output.bytes);
    }
};
