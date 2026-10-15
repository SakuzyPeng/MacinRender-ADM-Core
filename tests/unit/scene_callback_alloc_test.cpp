// Prepared Scene device callbacks must not allocate in C++. Rust kernels on
// the same path have their own allocation tests (realtime_allocations.rs).
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <malloc.h>
#endif

#include "../../src/adm_engine/scene_output_session.h"
#include "../../src/adm_render_common/hptf_eq.h"

// These replacements need CRT storage and shared counters; C++ containers would recurse into operator new.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables, cppcoreguidelines-no-malloc)
namespace {
std::atomic<std::size_t> g_callback_allocations{0};
thread_local bool t_armed = false;

void record_allocation() {
    if (t_armed) {
        g_callback_allocations.fetch_add(1U, std::memory_order_relaxed);
    }
}

void* allocate(std::size_t size) {
    record_allocation();
    if (void* memory = std::malloc(size == 0U ? 1U : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* allocate_aligned(std::size_t size, std::align_val_t alignment) {
    record_allocation();
    const auto bytes = size == 0U ? 1U : size;
#ifdef _WIN32
    void* memory = _aligned_malloc(bytes, static_cast<std::size_t>(alignment));
#else
    void* memory = nullptr;
    if (posix_memalign(&memory, static_cast<std::size_t>(alignment), bytes) != 0) {
        throw std::bad_alloc();
    }
#endif
    if (memory != nullptr) {
        return memory;
    }
    throw std::bad_alloc();
}

void free_aligned(void* memory) noexcept {
#ifdef _WIN32
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}
} // namespace

void* operator new(std::size_t size) {
    return allocate(size);
}
void* operator new[](std::size_t size) {
    return allocate(size);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocate_aligned(size, alignment);
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocate_aligned(size, alignment);
}
void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete[](void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::size_t /*size*/) noexcept {
    std::free(memory);
}
void operator delete[](void* memory, std::size_t /*size*/) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::align_val_t /*alignment*/) noexcept {
    free_aligned(memory);
}
void operator delete[](void* memory, std::align_val_t /*alignment*/) noexcept {
    free_aligned(memory);
}
void operator delete(void* memory, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
    free_aligned(memory);
}
void operator delete[](void* memory, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
    free_aligned(memory);
}
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables, cppcoreguidelines-no-malloc)

namespace {
using namespace mradm::realtime;

template <typename T> void require(const T& result) {
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
}

bool check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

template <typename Allocate, typename Release>
bool check_allocator(Allocate allocation_fn, Release release_fn, std::size_t alignment, std::string_view name) {
    bool ok = true;
    // Zero-size requests must succeed; an aligned size need not be a multiple of its alignment.
    for (const std::size_t size : {0U, 65U}) {
        g_callback_allocations.store(0U);
        t_armed = true;
        void* memory = allocation_fn(size);
        t_armed = false;
        ok &= check(g_callback_allocations.exchange(0U) == 1U, name);
        ok &= check(memory != nullptr, "allocator self-test returns storage");
        ok &= check(reinterpret_cast<std::uintptr_t>(memory) % alignment == 0U,
                    "allocator self-test preserves alignment");
        release_fn(memory, size);
        ok &= check(g_callback_allocations.load() == 0U, "unarmed deallocation does not affect the counter");
    }
    return ok;
}

bool check_allocation_counter() {
    // Call the functions directly: new-expressions may legally have their allocations elided.
    constexpr auto alignment = std::align_val_t{64U};
    constexpr auto ordinary_alignment = alignof(std::max_align_t);
    bool ok = true;
    ok &= check_allocator([](std::size_t size) { return ::operator new(size); },
                          [](void* memory, std::size_t) { ::operator delete(memory); },
                          ordinary_alignment,
                          "ordinary scalar allocation is counted");
    ok &= check_allocator([](std::size_t size) { return ::operator new[](size); },
                          [](void* memory, std::size_t) { ::operator delete[](memory); },
                          ordinary_alignment,
                          "ordinary array allocation is counted");
    ok &= check_allocator([](std::size_t size) { return ::operator new(size, alignment); },
                          [](void* memory, std::size_t) { ::operator delete(memory, alignment); },
                          64U,
                          "aligned scalar allocation is counted");
    ok &= check_allocator([](std::size_t size) { return ::operator new[](size, alignment); },
                          [](void* memory, std::size_t) { ::operator delete[](memory, alignment); },
                          64U,
                          "aligned array allocation is counted");
    ok &= check_allocator([](std::size_t size) { return ::operator new(size, alignment); },
                          [](void* memory, std::size_t size) { ::operator delete(memory, size, alignment); },
                          64U,
                          "aligned scalar allocation with sized delete is counted");
    ok &= check_allocator([](std::size_t size) { return ::operator new[](size, alignment); },
                          [](void* memory, std::size_t size) { ::operator delete[](memory, size, alignment); },
                          64U,
                          "aligned array allocation with sized delete is counted");
    // The standard nothrow forms must also reach the corresponding throwing replacement.
    ok &= check_allocator([](std::size_t size) { return ::operator new(size, std::nothrow); },
                          [](void* memory, std::size_t) { ::operator delete(memory, std::nothrow); },
                          ordinary_alignment,
                          "nothrow scalar allocation is counted");
    ok &= check_allocator([](std::size_t size) { return ::operator new[](size, std::nothrow); },
                          [](void* memory, std::size_t) { ::operator delete[](memory, std::nothrow); },
                          ordinary_alignment,
                          "nothrow array allocation is counted");
    ok &= check_allocator([](std::size_t size) { return ::operator new(size, alignment, std::nothrow); },
                          [](void* memory, std::size_t) { ::operator delete(memory, alignment, std::nothrow); },
                          64U,
                          "nothrow aligned scalar allocation is counted");
    ok &= check_allocator([](std::size_t size) { return ::operator new[](size, alignment, std::nothrow); },
                          [](void* memory, std::size_t) { ::operator delete[](memory, alignment, std::nothrow); },
                          64U,
                          "nothrow aligned array allocation is counted");
    return ok;
}

class CaptureDevice final : public IAudioOutputDevice {
  public:
    mradm::Result<void> start(std::uint32_t channels, std::uint32_t rate, PullFn callback) override {
        channels_ = channels;
        rate_ = rate;
        pull_ = std::move(callback);
        return {};
    }
    void stop() override { pull_ = {}; }
    [[nodiscard]] std::uint32_t actual_sample_rate() const override { return rate_; }
    // Only the callback itself is armed; the test thread prepares buffers.
    std::size_t pull(std::span<float> output) {
        t_armed = true;
        const auto produced = pull_(output, output.size() / channels_);
        t_armed = false;
        return produced;
    }

  private:
    PullFn pull_;
    std::uint32_t channels_{0U};
    std::uint32_t rate_{0U};
};

mradm::render_common::HptfCoefficients design(const char* text, std::uint32_t rate) {
    auto profile = mradm::render_common::parse_parametric_eq(text);
    require(profile);
    auto coeffs = mradm::render_common::design_cascade(*profile, rate, mradm::HptfPreampMode::warn_only);
    require(coeffs);
    return *coeffs;
}

// Runs one epoch through a device session; returns callback allocations.
// NOLINTNEXTLINE(readability-function-size): one epoch transaction including underrun recovery and EOS.
std::size_t run(mradm::RendererSelection renderer,
                const char* layout,
                std::uint32_t input_rate,
                std::uint32_t output_rate,
                bool protect) {
    SceneStreamConfig config;
    config.renderer.renderer = renderer;
    config.renderer.output_layout = layout;
    config.renderer.sample_rate = input_rate;
    config.output_sample_rate = output_rate;
    config.startup_watermark_frames = 1U;
    auto created = SceneStreamEngine::create(config);
    require(created);
    auto stream = std::shared_ptr<SceneStreamEngine>(std::move(*created));
    auto device = std::make_unique<CaptureDevice>();
    auto* capture = device.get();
    auto output = SceneOutputSession::create_with_device(stream, std::move(device), protect);
    require(output);
    const auto channels = stream->output_format().channels;
    const auto hptf_a = design("Preamp: -3 dB\nFilter 1: ON PK Fc 1700 Hz Gain 6 dB Q 0.75\n", output_rate);
    const auto hptf_b = design("Preamp: -6 dB\nFilter 1: ON LSC Fc 200 Hz Gain 9 dB Q 0.7\n", output_rate);
    require((*output)->begin_epoch(1U, 0));
    mradm::live_scene::ElementDescriptor element;
    element.element_id = 1U;
    require(stream->configure_generation(1U, 1U, std::span{&element, 1U}));
    mradm::live_scene::StateEntry initial;
    initial.element_id = 1U;
    initial.state.valid_fields = mradm::live_scene::k_known_state_fields;
    initial.state.x = 0.25F;
    initial.state.y = 0.75F;
    std::vector<float> samples(4096U);
    std::uint32_t seed = 0x2468ACE1U;
    for (auto& sample : samples) {
        seed = (seed * 1664525U) + 1013904223U;
        sample = 2.0F * (static_cast<float>(seed >> 8U) / 8388608.0F - 1.0F);
    }
    std::vector<float> callback(std::size_t{480U} * channels);
    std::int64_t position = 0;
    const auto submit = [&](bool first) {
        ScenePcmPlaneView plane{1U, samples.data(), static_cast<std::uint32_t>(samples.size()), 1U, true};
        SceneFrameView frame;
        frame.epoch_id = 1U;
        frame.generation_id = 1U;
        frame.media_sample_start = position;
        frame.duration_samples = static_cast<std::uint32_t>(samples.size());
        frame.flags = mradm::live_scene::frame_state_complete;
        frame.pcm = std::span{&plane, 1U};
        frame.initial_states = first ? std::span{&initial, 1U} : std::span<mradm::live_scene::StateEntry>{};
        require(stream->submit_frame(frame, std::chrono::seconds(5)));
        require(stream->wait_idle(std::chrono::seconds(20)));
        position += static_cast<std::int64_t>(samples.size());
    };
    (*output)->play();
    submit(true);
    // The first callback prepares nothing lazily; measure from the start.
    g_callback_allocations.store(0U);
    for (int block = 0; block < 6; ++block) {
        (void) capture->pull(callback);
    }
    if (protect) {
        require((*output)->set_hptf(hptf_a, 1));
        (void) capture->pull(callback);
        require((*output)->set_hptf(hptf_b, 2)); // queued behind the active fade
        require((*output)->set_volume(0.5F));
        (void) capture->pull(callback);
    } else {
        require((*output)->set_volume(0.5F));
    }
    // Drain into repeated underruns, then resume.
    for (int block = 0; block < 32; ++block) {
        (void) capture->pull(callback);
    }
    submit(false);
    require(stream->signal_end(1U, position));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!(*output)->status().ended) {
        if (capture->pull(callback) == 0U) {
            if (std::chrono::steady_clock::now() > deadline) {
                throw std::runtime_error("device EOS timed out");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if ((*output)->status().underruns == 0U) {
        throw std::runtime_error("allocation test did not exercise an underrun");
    }
    return g_callback_allocations.load();
}
} // namespace

int main() {
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    std::cout << "Diagnostic builds record checkpoints in callbacks; skipped\n";
    return 0;
#else
    try {
        bool ok = check_allocation_counter();
        ok &= check(run(mradm::RendererSelection::saf_binaural, "binaural", 48000U, 48000U, true) == 0U,
                    "binaural device callback with HpTF, volume and underruns does not allocate");
        ok &= check(run(mradm::RendererSelection::saf, "0+2+0", 44100U, 48000U, true) == 0U,
                    "resampled stereo device callback does not allocate");
        ok &= check(run(mradm::RendererSelection::saf, "4+7+0", 48000U, 48000U, false) == 0U,
                    "unprotected multichannel callback with Rust volume does not allocate");
        if (ok) {
            std::cout << "Scene device callbacks do not allocate\n";
        }
        return ok ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
#endif
}
