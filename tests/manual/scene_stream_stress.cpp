#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numbers>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <mach/mach.h>
#endif

#include "adm/c_api.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t k_sample_rate = 48000U;
constexpr std::uint32_t k_chunk_frames = 480U;
constexpr auto k_chunk_duration = std::chrono::milliseconds(10);
constexpr std::uint64_t k_rss_tolerance = 4ULL * 1024ULL * 1024ULL;

struct Options {
    std::uint32_t seconds{600U};
    std::uint32_t latency_iterations{500U};
    bool binaural{false};
};

struct StreamGuard {
    StreamGuard() = default;
    StreamGuard(const StreamGuard&) = delete;
    StreamGuard& operator=(const StreamGuard&) = delete;
    StreamGuard(StreamGuard&&) = delete;
    StreamGuard& operator=(StreamGuard&&) = delete;

    // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
    adm_scene_stream_t* value{nullptr};
    ~StreamGuard() { adm_destroy_scene_stream(value); }
};

[[nodiscard]] std::uint64_t current_rss_bytes() noexcept {
#ifdef __APPLE__
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    const auto status = task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count);
    return status == KERN_SUCCESS ? info.resident_size : 0U;
#else
    return 0U;
#endif
}

[[nodiscard]] bool parse_u32(std::string_view text, std::uint32_t& value) noexcept {
    std::uint32_t parsed = 0U;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return false;
    }
    value = parsed;
    return true;
}

[[nodiscard]] bool parse_options(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--seconds" || argument == "--latency-iterations" || argument == "--renderer") {
            if (index + 1 >= argc) {
                return false;
            }
            const std::string_view value{argv[++index]};
            if (argument == "--seconds") {
                if (!parse_u32(value, options.seconds) || options.seconds > 86400U) {
                    return false;
                }
            } else if (argument == "--latency-iterations") {
                if (!parse_u32(value, options.latency_iterations) || options.latency_iterations < 100U ||
                    options.latency_iterations > 100000U) {
                    return false;
                }
            } else if (value == "vbap") {
                options.binaural = false;
            } else if (value == "binaural") {
                options.binaural = true;
            } else {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] const char* renderer_name(bool binaural) noexcept {
    return binaural ? "binaural" : "vbap";
}

[[nodiscard]] adm_scene_stream_config_t make_config(bool binaural, bool latency_benchmark) noexcept {
    adm_scene_stream_config_t config{};
    config.struct_size = sizeof(config);
    config.renderer = binaural ? ADM_RENDERER_SAF_BINAURAL : ADM_RENDERER_SAF;
    config.output_layout = binaural ? "binaural" : "0+2+0";
    config.speaker_geometry = ADM_SPEAKER_GEOMETRY_STANDARD;
    config.speaker_spread_mode = ADM_SPEAKER_SPREAD_AUTOMATIC;
    config.binaural_spread_mode = ADM_BINAURAL_SPREAD_AUTOMATIC;
    config.lfe_routing_mode = ADM_LFE_ROUTING_DIRECT;
    config.input_sample_rate = k_sample_rate;
    config.output_sample_rate = k_sample_rate;
    config.input_queue_samples = latency_benchmark ? (2U * k_chunk_frames) : 65536U;
    config.input_queue_bytes = latency_benchmark ? (1ULL * 1024ULL * 1024ULL) : (8ULL * 1024ULL * 1024ULL);
    config.output_ring_frames = latency_benchmark ? 2048U : 32768U;
    config.startup_watermark_frames = latency_benchmark ? 1U : 9600U;
    return config;
}

[[nodiscard]] bool call_ok(adm_error_code_t code, const adm_scene_stream_t* stream, std::string_view operation) {
    if (code == ADM_ERROR_OK) {
        return true;
    }
    std::cerr << "FAIL: " << operation << " returned " << static_cast<int>(code);
    if (stream != nullptr) {
        std::cerr << ": " << adm_scene_stream_last_error_message(stream);
    }
    std::cerr << '\n';
    return false;
}

[[nodiscard]] bool create_stream(adm_context_t* context, bool binaural, bool latency_benchmark, StreamGuard& stream) {
    const auto config = make_config(binaural, latency_benchmark);
    return call_ok(adm_create_scene_stream(context, &config, &stream.value), stream.value, "create stream") &&
           stream.value != nullptr;
}

[[nodiscard]] bool begin_and_configure(adm_scene_stream_t* stream, std::uint64_t epoch) {
    if (!call_ok(adm_scene_stream_begin_epoch(stream, epoch, 0), stream, "begin epoch")) {
        return false;
    }
    adm_scene_element_descriptor_t descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.role = ADM_SCENE_ELEMENT_OBJECT;
    descriptor.element_id = 1U;
    descriptor.has_position = 1;
    descriptor.position_y = 1.0F;
    return call_ok(
        adm_scene_stream_configure_generation(stream, epoch, 1U, &descriptor, 1U), stream, "configure generation");
}

[[nodiscard]] adm_scene_object_state_t complete_state() noexcept {
    adm_scene_object_state_t state{};
    state.struct_size = sizeof(state);
    state.valid_fields = ADM_SCENE_STATE_ACTIVE | ADM_SCENE_STATE_LINEAR_GAIN | ADM_SCENE_STATE_POSITION |
                         ADM_SCENE_STATE_EXTENT | ADM_SCENE_STATE_DIFFUSE | ADM_SCENE_STATE_DIVERGENCE |
                         ADM_SCENE_STATE_CHANNEL_LOCK | ADM_SCENE_STATE_SCREEN_REFERENCE | ADM_SCENE_STATE_HEAD_LOCKED;
    state.active = 1;
    state.linear_gain = 0.75F;
    state.position_y = 1.0F;
    return state;
}

class FramePayload {
  public:
    FramePayload() : samples_(k_chunk_frames) {
        constexpr float frequency = 997.0F;
        for (std::size_t frame = 0U; frame < samples_.size(); ++frame) {
            const float phase = 2.0F * std::numbers::pi_v<float> * frequency * static_cast<float>(frame) /
                                static_cast<float>(k_sample_rate);
            samples_[frame] = 0.05F * std::sin(phase);
        }

        plane_.struct_size = sizeof(plane_);
        plane_.element_id = 1U;
        plane_.samples = samples_.data();
        plane_.sample_count = k_chunk_frames;
        plane_.stride = 1U;
        plane_.has_signal = 1;

        initial_.struct_size = sizeof(initial_);
        initial_.element_id = 1U;
        initial_.state = complete_state();

        update_.struct_size = sizeof(update_);
        update_.element_id = 1U;
        update_.offset_samples = k_chunk_frames / 2U;
        update_.ramp_duration_samples = k_chunk_frames;
        update_.changed_fields = ADM_SCENE_STATE_LINEAR_GAIN | ADM_SCENE_STATE_POSITION;
        update_.state = complete_state();

        frame_.struct_size = sizeof(frame_);
        frame_.flags = ADM_SCENE_FRAME_STATE_COMPLETE;
        frame_.generation_id = 1U;
        frame_.duration_samples = k_chunk_frames;
        frame_.pcm_count = 1U;
        frame_.pcm = &plane_;
        frame_.metadata_update_count = 1U;
        frame_.metadata_updates = &update_;
    }

    [[nodiscard]] const adm_scene_frame_t* prepare(std::uint64_t epoch, std::int64_t start, bool first_frame) noexcept {
        frame_.epoch_id = epoch;
        frame_.media_sample_start = start;
        frame_.initial_state_count = first_frame ? 1U : 0U;
        frame_.initial_states = first_frame ? &initial_ : nullptr;
        const float modulation = std::sin((static_cast<float>(start) / static_cast<float>(k_chunk_frames)) * 0.013F);
        update_.state.linear_gain = 0.75F + (0.2F * modulation);
        update_.state.position_x = 0.75F * modulation;
        update_.state.position_y = 0.65F;
        return &frame_;
    }

  private:
    std::vector<float> samples_;
    adm_scene_pcm_plane_t plane_{};
    adm_scene_initial_state_t initial_{};
    adm_scene_metadata_update_t update_{};
    adm_scene_frame_t frame_{};
};

[[nodiscard]] bool query_status(adm_scene_stream_t* stream, adm_scene_stream_status_t& status) {
    status = {};
    status.struct_size = sizeof(status);
    return call_ok(adm_scene_stream_get_status(stream, &status), stream, "get status");
}

[[nodiscard]] double percentile_99(std::vector<double> samples) {
    if (samples.empty()) {
        return 0.0;
    }
    std::ranges::sort(samples);
    const std::size_t rank = ((samples.size() * 99U) + 99U) / 100U;
    return samples[std::max<std::size_t>(1U, rank) - 1U];
}

[[nodiscard]] bool
pull_frames(adm_scene_stream_t* stream, std::vector<float>& output, adm_scene_pull_result_t& result) {
    result = {};
    result.struct_size = sizeof(result);
    return call_ok(adm_scene_stream_pull(stream, output.data(), k_chunk_frames, &result), stream, "pull");
}

[[nodiscard]] bool drain_to_eos(adm_scene_stream_t* stream) {
    std::vector<float> output(static_cast<std::size_t>(k_chunk_frames) * 2U);
    for (std::uint32_t attempt = 0U; attempt < 10000U; ++attempt) {
        adm_scene_pull_result_t pulled{};
        if (!pull_frames(stream, output, pulled)) {
            return false;
        }
        if ((pulled.flags & ADM_SCENE_PULL_FAILED) != 0U) {
            std::cerr << "FAIL: stream failed while draining\n";
            return false;
        }
        if ((pulled.flags & ADM_SCENE_PULL_EOS) != 0U) {
            return true;
        }
        if (pulled.media_frames == 0U) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    std::cerr << "FAIL: stream did not reach EOS\n";
    return false;
}

[[nodiscard]] bool benchmark_worker_completion(adm_context_t* context, bool binaural, std::uint32_t iterations) {
    StreamGuard stream;
    if (!create_stream(context, binaural, true, stream) || !begin_and_configure(stream.value, 1U)) {
        return false;
    }

    FramePayload payload;
    std::vector<float> output(static_cast<std::size_t>(k_chunk_frames) * 2U);
    std::vector<double> latencies_us;
    latencies_us.reserve(iterations);
    std::int64_t next_sample = 0;
    for (std::uint32_t iteration = 0U; iteration < iterations; ++iteration) {
        const auto started = Clock::now();
        int32_t submit_status = ADM_SCENE_SUBMIT_CLOSED;
        if (!call_ok(adm_scene_stream_submit_frame(
                         stream.value, payload.prepare(1U, next_sample, iteration == 0U), 1000U, &submit_status),
                     stream.value,
                     "benchmark submit") ||
            submit_status != ADM_SCENE_SUBMIT_ACCEPTED) {
            std::cerr << "FAIL: benchmark frame was not accepted\n";
            return false;
        }

        bool ready = false;
        while (Clock::now() - started < std::chrono::seconds(1)) {
            adm_scene_stream_status_t status{};
            if (!query_status(stream.value, status)) {
                return false;
            }
            if (status.failed != 0) {
                std::cerr << "FAIL: worker failed during latency benchmark\n";
                return false;
            }
            if (status.buffered_output_frames >= k_chunk_frames) {
                ready = true;
                break;
            }
            std::this_thread::yield();
        }
        if (!ready) {
            std::cerr << "FAIL: worker did not complete a 10 ms block within one second\n";
            return false;
        }
        const auto finished = Clock::now();
        adm_scene_pull_result_t pulled{};
        if (!pull_frames(stream.value, output, pulled) || pulled.media_frames != k_chunk_frames || pulled.flags != 0U) {
            std::cerr << "FAIL: benchmark pull did not return one complete media block\n";
            return false;
        }
        latencies_us.push_back(std::chrono::duration<double, std::micro>(finished - started).count());
        next_sample += k_chunk_frames;
    }

    if (!call_ok(adm_scene_stream_signal_end(stream.value, 1U, next_sample), stream.value, "benchmark EOS") ||
        !drain_to_eos(stream.value)) {
        return false;
    }
    const double p99_us = percentile_99(std::move(latencies_us));
    std::cout << "worker renderer=" << renderer_name(binaural) << " blocks=" << iterations
              << " completion_p99_us=" << p99_us << " budget_us=10000\n";
    if (p99_us >= 10000.0) {
        std::cerr << "FAIL: worker completion p99 exceeds one input block budget\n";
        return false;
    }
    return true;
}

struct StatusSample {
    std::uint64_t rss_bytes{0U};
};

[[nodiscard]] std::uint64_t
average_rss(const std::vector<StatusSample>& samples, std::size_t first, std::size_t count) noexcept {
    if (samples.empty() || first >= samples.size() || count == 0U) {
        return 0U;
    }
    const std::size_t last = std::min(samples.size(), first + count);
    const auto begin = samples.begin() + static_cast<std::ptrdiff_t>(first);
    const auto end = samples.begin() + static_cast<std::ptrdiff_t>(last);
    const std::uint64_t sum =
        std::accumulate(begin, end, std::uint64_t{0U}, [](std::uint64_t total, const StatusSample& sample) {
            return total + sample.rss_bytes;
        });
    return sum / (last - first);
}

// One producer, one real-time pull thread (this function), and one status sampler exercise the
// published concurrency contract for a sustained wall-clock interval.
// NOLINTNEXTLINE(readability-function-size)
[[nodiscard]] bool run_continuous(adm_context_t* context, bool binaural, std::uint32_t seconds) {
    StreamGuard stream;
    if (!create_stream(context, binaural, false, stream) || !begin_and_configure(stream.value, 2U)) {
        return false;
    }

    std::atomic<bool> stop_producer{false};
    bool producer_failed = false;
    std::string producer_error;
    std::int64_t producer_next_sample = 0;
    std::thread producer([&] {
        FramePayload payload;
        bool first_frame = true;
        while (!stop_producer.load(std::memory_order_acquire)) {
            int32_t submit_status = ADM_SCENE_SUBMIT_CLOSED;
            const auto code = adm_scene_stream_submit_frame(
                stream.value, payload.prepare(2U, producer_next_sample, first_frame), 50U, &submit_status);
            if (code != ADM_ERROR_OK) {
                producer_failed = true;
                producer_error = adm_scene_stream_last_error_message(stream.value);
                return;
            }
            if (submit_status == ADM_SCENE_SUBMIT_ACCEPTED) {
                producer_next_sample += k_chunk_frames;
                first_frame = false;
            } else if (submit_status == ADM_SCENE_SUBMIT_CLOSED) {
                if (!stop_producer.load(std::memory_order_acquire)) {
                    producer_failed = true;
                    producer_error = "stream closed before the stress run stopped";
                }
                return;
            }
        }
    });

    bool startup_ready = false;
    const auto startup_deadline = Clock::now() + std::chrono::seconds(15);
    while (Clock::now() < startup_deadline) {
        adm_scene_stream_status_t status{};
        if (!query_status(stream.value, status) || status.failed != 0) {
            break;
        }
        if (status.buffered_output_frames >= 9600U) {
            startup_ready = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!startup_ready) {
        stop_producer.store(true, std::memory_order_release);
        producer.join();
        std::cerr << "FAIL: stream did not reach its startup watermark\n";
        return false;
    }

    std::atomic<bool> stop_sampler{false};
    std::vector<StatusSample> status_samples;
    status_samples.reserve(static_cast<std::size_t>(seconds) + 4U);
    std::thread sampler([&] {
        while (!stop_sampler.load(std::memory_order_acquire)) {
            adm_scene_stream_status_t status{};
            if (query_status(stream.value, status)) {
                status_samples.push_back({current_rss_bytes()});
            }
            for (int slice = 0; slice < 10 && !stop_sampler.load(std::memory_order_acquire); ++slice) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        adm_scene_stream_status_t status{};
        if (query_status(stream.value, status)) {
            status_samples.push_back({current_rss_bytes()});
        }
    });

    const std::uint64_t pull_count = static_cast<std::uint64_t>(seconds) * 100U;
    std::vector<float> output(static_cast<std::size_t>(k_chunk_frames) * 2U);
    std::vector<double> pull_latencies_us;
    pull_latencies_us.reserve(static_cast<std::size_t>(pull_count));
    bool pull_failed = false;
    auto next_tick = Clock::now();
    for (std::uint64_t pull_index = 0U; pull_index < pull_count; ++pull_index) {
        const auto started = Clock::now();
        adm_scene_pull_result_t pulled{};
        if (!pull_frames(stream.value, output, pulled) || pulled.media_frames != k_chunk_frames || pulled.flags != 0U) {
            std::cerr << "FAIL: real-time pull " << pull_index << " returned flags=" << pulled.flags
                      << " media_frames=" << pulled.media_frames << '\n';
            pull_failed = true;
            break;
        }
        pull_latencies_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - started).count());
        next_tick += k_chunk_duration;
        std::this_thread::sleep_until(next_tick);
    }

    stop_producer.store(true, std::memory_order_release);
    producer.join();
    stop_sampler.store(true, std::memory_order_release);
    sampler.join();

    adm_scene_stream_status_t status{};
    const bool have_status = query_status(stream.value, status);
    const auto expected_media_frames = pull_count * k_chunk_frames;
    bool ok = !pull_failed && !producer_failed && have_status && status.failed == 0 && status.underruns == 0U &&
              status.media_frames_pulled == expected_media_frames;
    if (producer_failed) {
        std::cerr << "FAIL: producer: " << producer_error << '\n';
    }
    if (have_status && (status.underruns != 0U || status.failed != 0)) {
        std::cerr << "FAIL: final status underruns=" << status.underruns << " failed=" << status.failed << '\n';
    }

    const double pull_p99_us = percentile_99(std::move(pull_latencies_us));
    if (pull_p99_us >= 10000.0) {
        std::cerr << "FAIL: pull p99 exceeds one device callback budget\n";
        ok = false;
    }

    std::uint64_t rss_baseline = 0U;
    std::uint64_t rss_end = 0U;
    std::uint64_t rss_peak = 0U;
    if (!status_samples.empty() && status_samples.front().rss_bytes != 0U) {
        const std::size_t baseline_index = std::min<std::size_t>(30U, status_samples.size() - 1U);
        rss_baseline = average_rss(status_samples, baseline_index, 5U);
        const std::size_t end_count = std::min<std::size_t>(5U, status_samples.size());
        rss_end = average_rss(status_samples, status_samples.size() - end_count, end_count);
        const auto steady_begin = status_samples.begin() + static_cast<std::ptrdiff_t>(baseline_index);
        rss_peak = std::accumulate(
            steady_begin, status_samples.end(), std::uint64_t{0U}, [](std::uint64_t peak, const StatusSample& sample) {
                return std::max(peak, sample.rss_bytes);
            });
        if (rss_end > rss_baseline + k_rss_tolerance) {
            std::cerr << "FAIL: steady-state RSS grew by more than 4 MiB\n";
            ok = false;
        }
    }

    std::cout << "continuous renderer=" << renderer_name(binaural) << " seconds=" << seconds
              << " media_frames=" << status.media_frames_pulled << " underruns=" << status.underruns
              << " pull_p99_us=" << pull_p99_us << " rss_baseline=" << rss_baseline << " rss_end=" << rss_end
              << " rss_peak=" << rss_peak << '\n';

    if (!call_ok(adm_scene_stream_signal_end(stream.value, 2U, producer_next_sample), stream.value, "stress EOS") ||
        !drain_to_eos(stream.value)) {
        ok = false;
    }
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        std::cerr << "usage: mr_adm_scene_stream_stress [--renderer vbap|binaural] [--seconds 0..86400] "
                     "[--latency-iterations 100..100000]\n";
        return 2;
    }

    adm_context_t* context = adm_create_context();
    if (context == nullptr) {
        std::cerr << "FAIL: could not create C API context\n";
        return 1;
    }
    bool ok = benchmark_worker_completion(context, options.binaural, options.latency_iterations);
    if (ok && options.seconds > 0U) {
        ok = run_continuous(context, options.binaural, options.seconds);
    }
    adm_destroy_context(context);
    return ok ? 0 : 1;
}
