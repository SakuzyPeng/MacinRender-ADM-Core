// spatialaudioclient_device.cpp
//
// Windows-only IAudioOutputDevice backed by ISpatialAudioClient. Instead of opening a raw
// multichannel hardware device (the miniaudio path), it submits the monitor's multichannel bed as
// spatial-audio objects (one per channel) and the active Windows spatializer (Windows Sonic for
// Headphones / Dolby Atmos for Headphones / DTS Headphone:X) HRTF-renders them to the headphone
// route. Most channels map to named *static* bed slots; layouts above the 8.1.4.4 static ceiling
// route extra speaker positions as *dynamic* objects pinned in space with SetPosition
// (see windows_layouts.h). This is the Windows analog of
// adm_apple/avsamplebuffer_device.mm.
//
// Unlike AVSampleBufferAudioRenderer (a buffered push renderer needing prefill/stall machinery),
// ISpatialAudioClient is an event-driven *pull*: a dedicated render thread waits on the stream's
// buffer-completion event and fills the object buffers each pass — the same realtime-callback model
// as miniaudio. So pull_is_realtime_playback() is true and flush/pause/resume stay no-ops (the
// engine feeds silence through pull() while paused). Windows COM / SpatialAudio types stay confined
// to adm_windows (ADR 0003); the factory returns the third-party-free IAudioOutputDevice.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <future>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// clang-format off
#include <windows.h>
#include <mmdeviceapi.h>
#include <spatialaudioclient.h>
#include <wrl/client.h>
// clang-format on

#include "adm/errors.h"

#include "audio_output_device.h"
#include "windows_layouts.h"

namespace mradm::realtime {
namespace {

using Microsoft::WRL::ComPtr;

constexpr float k_lfe_pair_gain = 0.70710678F;
constexpr float k_lfe_active_epsilon = 1.0e-12F;

// Build an error (carrying the failing HRESULT in hex) ready to return into a Result<>. Mirrors
// make_error's tl::unexpected<Error> return so callers can `return hr_error(...)` directly.
[[nodiscard]] tl::unexpected<Error>
hr_error(ErrorCode code, std::string message, const std::string& where, HRESULT hr) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return make_error(code, std::move(message), where + " hr=" + buf);
}

class SpatialAudioClientDevice final : public IAudioOutputDevice {
  public:
    explicit SpatialAudioClientDevice(std::string layout_id) : layout_id_(std::move(layout_id)) {}
    ~SpatialAudioClientDevice() override { stop(); }

    [[nodiscard]] Result<void> start(uint32_t channels, uint32_t sample_rate, PullFn pull) override {
        const auto* layout = windows_layouts::find_windows_speaker_layout(layout_id_);
        if (layout == nullptr) {
            return make_error(ErrorCode::unsupported, "系统空间音频不支持该监听布局", "layout=" + layout_id_);
        }
        if (channels != layout->channels) {
            return make_error(ErrorCode::invalid_argument,
                              "监听声道数与布局不符",
                              "layout=" + layout_id_ + " channels=" + std::to_string(channels));
        }
        channels_ = channels;
        sample_rate_ = sample_rate;
        pull_ = std::move(pull);
        layout_ = layout;
        stop_.store(false, std::memory_order_release);

        // The buffer-completion event lives for the device's whole lifetime (closed in stop() after
        // join). The render thread reuses it across stream rebuilds — when the user switches the
        // system spatializer the stream is invalidated and re-created, but the event persists — so it
        // is not tied to any single ISpatialAudioObjectRenderStream.
        buffer_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (buffer_event_ == nullptr) {
            pull_ = nullptr;
            return make_error(ErrorCode::render_failed, "创建空间音频缓冲事件失败", "system-spatial");
        }

        // All COM work (activation + the render pump) lives on render_thread_ so the apartment is
        // consistent. start() blocks on a promise the thread fulfils once setup either succeeds or
        // fails, so activation errors (e.g. no spatial format enabled) surface synchronously here.
        std::promise<Result<void>> ready;
        std::future<Result<void>> ready_future = ready.get_future();
        render_thread_ = std::thread(&SpatialAudioClientDevice::render_loop, this, std::move(ready));
        Result<void> setup = ready_future.get();
        if (!setup) {
            if (render_thread_.joinable()) {
                render_thread_.join();
            }
            pull_ = nullptr;
        }
        return setup;
    }

    void stop() override {
        stop_.store(true, std::memory_order_release);
        if (buffer_event_ != nullptr) {
            SetEvent(buffer_event_); // wake the pump so it can observe stop_
        }
        if (render_thread_.joinable()) {
            render_thread_.join();
        }
        // Close the event only here, after the render thread is joined — the render thread never
        // touches it during teardown, so stop()'s SetEvent above can't race a concurrent close.
        if (buffer_event_ != nullptr) {
            CloseHandle(buffer_event_);
            buffer_event_ = nullptr;
        }
        pull_ = nullptr;
    }

    [[nodiscard]] uint32_t actual_sample_rate() const override { return sample_rate_; }
    [[nodiscard]] bool pull_is_realtime_playback() const override { return true; }

  private:
    enum class PumpResult { Stopped, Invalidated };

    // Owns COM init + the spatial stream for its whole lifetime; reports the first setup result
    // through `ready`, then pumps until stop_. If the stream is invalidated mid-session (the user
    // switches the system spatializer — Dolby/Sonic/DTS — or the endpoint reconfigures), rebuilds it
    // transparently and resumes, so monitoring survives a spatializer change without a manual restart.
    void render_loop(std::promise<Result<void>> ready) {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool co_owned = SUCCEEDED(co);

        Result<void> setup = setup_stream();
        if (!setup) {
            teardown();
            if (co_owned) {
                CoUninitialize();
            }
            ready.set_value(std::move(setup));
            return;
        }
        ready.set_value({});

        while (pump() == PumpResult::Invalidated && !stop_.load(std::memory_order_acquire)) {
            teardown();
            // Let the spatializer switch settle before rebuilding; also bounds the rate if a freshly
            // rebuilt stream gets invalidated again while the switch is still in progress.
            Sleep(100);
            // The spatializer can be momentarily unavailable mid-switch; retry the rebuild (grabbing
            // the now-current default endpoint + spatializer) until it succeeds or the user stops.
            while (!stop_.load(std::memory_order_acquire) && !setup_stream()) {
                teardown();
                Sleep(150);
            }
        }

        teardown();
        if (co_owned) {
            CoUninitialize();
        }
    }

    [[nodiscard]] Result<void> setup_stream() {
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) {
            return hr_error(ErrorCode::render_failed, "创建 MMDeviceEnumerator 失败", "system-spatial", hr);
        }
        ComPtr<IMMDevice> endpoint;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint);
        if (FAILED(hr)) {
            return hr_error(ErrorCode::render_failed, "获取默认音频输出端点失败", "system-spatial", hr);
        }
        hr = endpoint->Activate(__uuidof(ISpatialAudioClient), CLSCTX_INPROC_SERVER, nullptr, &spatial_client_);
        if (FAILED(hr)) {
            return hr_error(ErrorCode::unsupported,
                            "当前输出设备不支持空间音频(请在 Windows 声音设置中启用 Windows Sonic / "
                            "Dolby Atmos / DTS 头戴空间音频)",
                            "system-spatial",
                            hr);
        }

        object_format_ = WAVEFORMATEX{};
        object_format_.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        object_format_.nChannels = 1; // each static/dynamic spatial object is mono
        object_format_.nSamplesPerSec = sample_rate_;
        object_format_.wBitsPerSample = 32;
        object_format_.nBlockAlign = static_cast<WORD>(object_format_.nChannels * object_format_.wBitsPerSample / 8);
        object_format_.nAvgBytesPerSec = object_format_.nSamplesPerSec * object_format_.nBlockAlign;
        object_format_.cbSize = 0;

        hr = spatial_client_->IsAudioObjectFormatSupported(&object_format_);
        if (FAILED(hr)) {
            // The active spatializer only accepts 48 kHz float mono objects; report the actual rate
            // so a non-48 kHz monitor rate isn't hidden behind a hardcoded "48 kHz" message.
            return hr_error(ErrorCode::unsupported,
                            "系统空间音频不支持该对象格式(float mono)",
                            "system-spatial sample_rate=" + std::to_string(sample_rate_) + "Hz",
                            hr);
        }

        ResetEvent(buffer_event_); // clear any stale signal from a prior (invalidated) stream

        // Static bed slots come for free via StaticObjectTypeMask; layouts that need extra speaker
        // positions additionally request dynamic objects. Min == Max
        // == the needed count so activation fails up front if the spatializer can't provide them
        // (returned as unsupported below) rather than the pump discovering it mid-stream and spinning
        // on rebuilds. All three spatializers report ample dynamic capacity (Dolby 16 / DTS 32 /
        // Sonic 111), so the 4 objects 9.1.6 needs and the 11 objects 22.2 needs fit the tested
        // consumer headphone spatializers.
        const uint32_t dynamic_count = windows_layouts::dynamic_object_count(*layout_);

        SpatialAudioObjectRenderStreamActivationParams params{};
        params.ObjectFormat = &object_format_;
        params.StaticObjectTypeMask = windows_layouts::static_object_mask(*layout_);
        params.MinDynamicObjectCount = dynamic_count;
        params.MaxDynamicObjectCount = dynamic_count;
        params.Category = AudioCategory_Media;
        params.EventHandle = buffer_event_;
        params.NotifyObject = nullptr;

        PROPVARIANT activation{};
        PropVariantInit(&activation);
        activation.vt = VT_BLOB;
        activation.blob.cbSize = sizeof(params);
        activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

        hr = spatial_client_->ActivateSpatialAudioStream(&activation, IID_PPV_ARGS(&render_stream_));
        if (FAILED(hr)) {
            return hr_error(ErrorCode::unsupported, "激活空间音频流失败(可能未启用空间音频格式)", "system-spatial", hr);
        }

        const std::size_t object_count = windows_layouts::object_count(*layout_);
        objects_.assign(object_count, nullptr);
        object_routes_.assign(object_count, {});
        object_sources_.assign(object_count, {});
        for (std::size_t ch = 0; ch < layout_->routes.size(); ++ch) {
            const windows_layouts::ChannelRoute& route = layout_->routes[ch];
            const std::size_t slot = windows_layouts::object_slot(route, ch);
            if (object_routes_[slot].type == AudioObjectType_None) {
                object_routes_[slot] = route;
            }
            object_sources_[slot].push_back(ch);
        }
        scratch_.assign(0, 0.0F);

        hr = render_stream_->Start();
        if (FAILED(hr)) {
            return hr_error(ErrorCode::render_failed, "启动空间音频流失败", "system-spatial", hr);
        }
        return {};
    }

    PumpResult pump() {
        // Pairs every successful BeginUpdatingAudioObjects with EndUpdatingAudioObjects on any scope
        // exit (normal completion, fc==0 continue, or an Invalidated return mid-cycle), per the API's
        // Begin/End contract — even when we're about to tear the stream down.
        struct EndGuard {
            ISpatialAudioObjectRenderStream* stream;
            ~EndGuard() { stream->EndUpdatingAudioObjects(); }
        };
        while (!stop_.load(std::memory_order_acquire)) {
            // The event throttles the loop (it fires ~every 10 ms when the system wants data); the
            // 100 ms timeout is a safety net so we still probe the stream if the event stops firing
            // (which it does once the spatializer changes and the stream is invalidated).
            WaitForSingleObject(buffer_event_, 100);
            if (stop_.load(std::memory_order_acquire)) {
                return PumpResult::Stopped;
            }

            UINT32 available_dynamic = 0;
            UINT32 frame_count = 0;
            HRESULT hr = render_stream_->BeginUpdatingAudioObjects(&available_dynamic, &frame_count);
            // Any failure means the stream is no longer usable — ask the render loop to rebuild it.
            // Switching the system spatializer (Dolby/Sonic/DTS) makes the buffer event go quiet and
            // Begin return a stream-lost error (observed 0x88890100, also RESOURCES_INVALIDATED /
            // STREAM_NOT_AVAILABLE); the exact code varies, so trigger on any FAILED. In the healthy
            // state the event fires ~every 10 ms and Begin (called right after) always succeeds, so a
            // failure here only happens when the stream is genuinely gone (a rare false trigger just
            // rebuilds and self-heals). Calling Begin even on the 100 ms timeout is what lets us
            // notice once the event has stopped firing.
            if (FAILED(hr)) {
                return PumpResult::Invalidated; // Begin failed → no End to pair
            }
            // Begin succeeded: from here every exit path must End the cycle.
            const EndGuard end_guard{render_stream_.Get()};
            if (frame_count == 0) {
                continue;
            }

            // Pull one interleaved block from the engine's ring; zero-pad a short read so a drained
            // ring plays silence rather than stale samples.
            const std::size_t need = static_cast<std::size_t>(frame_count) * channels_;
            if (scratch_.size() < need) {
                scratch_.resize(need);
            }
            const std::size_t produced = pull_ ? pull_(std::span<float>(scratch_.data(), need), frame_count) : 0;
            if (produced < frame_count) {
                std::fill(scratch_.begin() + static_cast<std::ptrdiff_t>(produced * channels_),
                          scratch_.begin() + static_cast<std::ptrdiff_t>(need),
                          0.0F);
            }

            // Deinterleave/mix into each object's mono buffer. Objects (static bed slots and dynamic
            // virtual speakers alike) persist for the stream's lifetime, so activate each once
            // (lazily) and reuse the ComPtr thereafter — keeping GetBuffer fed every pass is what
            // keeps them alive. A failure here means the stream is no longer usable (it can also be
            // invalidated *after* Begin succeeded — e.g. a spatializer switch landing mid-cycle).
            // Treat it as invalidation and rebuild rather than silently dropping the channel while
            // monitoring looks healthy. Static types are always a subset of the native 8.1.4.4 mask
            // and the dynamic count was reserved at activation, so activation never fails for a
            // genuine config reason — only a dead stream does.
            for (std::size_t object_index = 0; object_index < objects_.size(); ++object_index) {
                const windows_layouts::ChannelRoute& route = object_routes_[object_index];
                if (objects_[object_index] == nullptr) {
                    hr = render_stream_->ActivateSpatialAudioObject(route.type, &objects_[object_index]);
                    if (FAILED(hr) || objects_[object_index] == nullptr) {
                        objects_[object_index] = nullptr;
                        return PumpResult::Invalidated;
                    }
                    // A dynamic object is a fixed virtual speaker, so set its position once right
                    // after activation; it persists "until changed" (we never move it). Static bed
                    // slots ignore position (the spatializer owns their geometry).
                    if (route.is_dynamic) {
                        hr = objects_[object_index]->SetPosition(route.x, route.y, route.z);
                        if (FAILED(hr)) {
                            objects_[object_index] = nullptr;
                            return PumpResult::Invalidated;
                        }
                    }
                }
                BYTE* buffer = nullptr;
                UINT32 buffer_bytes = 0;
                hr = objects_[object_index]->GetBuffer(&buffer, &buffer_bytes);
                if (FAILED(hr) || buffer == nullptr) {
                    return PumpResult::Invalidated;
                }
                const UINT32 want_bytes = frame_count * static_cast<UINT32>(sizeof(float));
                const UINT32 copy_frames =
                    (buffer_bytes < want_bytes ? buffer_bytes : want_bytes) / static_cast<UINT32>(sizeof(float));
                auto* dst = reinterpret_cast<float*>(buffer);
                const std::vector<std::size_t>& sources = object_sources_[object_index];
                if (route.type == AudioObjectType_LowFrequency && sources.size() == 2U) {
                    // 22.2 folds LFE1/LFE2 into the one Windows LFE object; attenuate only when
                    // both source channels are active in this render block.
                    const auto source_has_energy = [&](std::size_t ch) {
                        for (UINT32 f = 0; f < copy_frames; ++f) {
                            if (std::abs(scratch_[static_cast<std::size_t>(f) * channels_ + ch]) >
                                k_lfe_active_epsilon) {
                                return true;
                            }
                        }
                        return false;
                    };
                    const bool attenuate_sum = source_has_energy(sources[0]) && source_has_energy(sources[1]);
                    for (UINT32 f = 0; f < copy_frames; ++f) {
                        const std::size_t frame_offset = static_cast<std::size_t>(f) * channels_;
                        const float a = scratch_[frame_offset + sources[0]];
                        const float b = scratch_[frame_offset + sources[1]];
                        dst[f] = attenuate_sum ? (a + b) * k_lfe_pair_gain : (a + b);
                    }
                } else if (sources.size() == 1U) {
                    const std::size_t ch = sources.front();
                    for (UINT32 f = 0; f < copy_frames; ++f) {
                        dst[f] = scratch_[static_cast<std::size_t>(f) * channels_ + ch];
                    }
                } else {
                    for (UINT32 f = 0; f < copy_frames; ++f) {
                        const std::size_t frame_offset = static_cast<std::size_t>(f) * channels_;
                        float sample = 0.0F;
                        for (const std::size_t ch : sources) {
                            sample += scratch_[frame_offset + ch];
                        }
                        dst[f] = sample;
                    }
                }
            }
            // end_guard ends the cycle on scope exit.
        }
        return PumpResult::Stopped;
    }

    // Releases the COM stream/objects (runs on the render thread). Deliberately does NOT close
    // buffer_event_ — stop() owns the handle's lifetime and closes it after joining this thread, so
    // stop()'s SetEvent can never race a close here. On the setup-failure path the destructor's
    // stop() closes it after the (already-finished) thread is joined.
    void teardown() {
        if (render_stream_ != nullptr) {
            render_stream_->Stop();
            render_stream_->Reset();
        }
        objects_.clear();
        object_routes_.clear();
        object_sources_.clear();
        render_stream_.Reset();
        spatial_client_.Reset();
    }

    std::string layout_id_;
    const windows_layouts::WindowsSpeakerLayout* layout_{nullptr};
    uint32_t channels_{0};
    uint32_t sample_rate_{0};
    PullFn pull_;

    std::thread render_thread_;
    std::atomic<bool> stop_{false};
    HANDLE buffer_event_{nullptr};
    WAVEFORMATEX object_format_{};
    ComPtr<ISpatialAudioClient> spatial_client_;
    ComPtr<ISpatialAudioObjectRenderStream> render_stream_;
    std::vector<ComPtr<ISpatialAudioObject>> objects_;
    std::vector<windows_layouts::ChannelRoute> object_routes_;
    std::vector<std::vector<std::size_t>> object_sources_;
    std::vector<float> scratch_; // interleaved pull staging, channels_ wide
};

} // namespace

std::unique_ptr<IAudioOutputDevice> make_spatialaudioclient_device(std::string layout_id) {
    return std::make_unique<SpatialAudioClientDevice>(std::move(layout_id));
}

std::vector<SystemSpatialLayoutInfo> system_spatial_layouts() {
    std::vector<SystemSpatialLayoutInfo> out;
    out.reserve(windows_layouts::k_windows_speaker_layouts.size());
    for (const auto& layout : windows_layouts::k_windows_speaker_layouts) {
        out.push_back({std::string(layout.id), std::string(layout.display_name), layout.channels});
    }
    return out;
}

} // namespace mradm::realtime
