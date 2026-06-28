// spatialaudioclient_device.cpp
//
// Windows-only IAudioOutputDevice backed by ISpatialAudioClient. Instead of opening a raw
// multichannel hardware device (the miniaudio path), it submits the monitor's multichannel bed as a
// set of *static* spatial-audio objects (one per channel), and the active Windows spatializer
// (Windows Sonic for Headphones / Dolby Atmos for Headphones / DTS Headphone:X) HRTF-renders the
// bed to the headphone route. This is the Windows analog of adm_apple/avsamplebuffer_device.mm.
//
// Unlike AVSampleBufferAudioRenderer (a buffered push renderer needing prefill/stall machinery),
// ISpatialAudioClient is an event-driven *pull*: a dedicated render thread waits on the stream's
// buffer-completion event and fills the object buffers each pass — the same realtime-callback model
// as miniaudio. So pull_is_realtime_playback() is true and flush/pause/resume stay no-ops (the
// engine feeds silence through pull() while paused). Windows COM / SpatialAudio types stay confined
// to adm_windows (ADR 0003); the factory returns the third-party-free IAudioOutputDevice.

#include <algorithm>
#include <atomic>
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
    // Owns COM init + the spatial stream for its whole lifetime; reports setup result through
    // `ready`, then pumps until stop_.
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

        pump();

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
        object_format_.nChannels = 1; // each static object is a mono bed channel
        object_format_.nSamplesPerSec = sample_rate_;
        object_format_.wBitsPerSample = 32;
        object_format_.nBlockAlign = static_cast<WORD>(object_format_.nChannels * object_format_.wBitsPerSample / 8);
        object_format_.nAvgBytesPerSec = object_format_.nSamplesPerSec * object_format_.nBlockAlign;
        object_format_.cbSize = 0;

        hr = spatial_client_->IsAudioObjectFormatSupported(&object_format_);
        if (FAILED(hr)) {
            return hr_error(ErrorCode::unsupported, "系统空间音频不支持该对象格式(48 kHz float)", "system-spatial", hr);
        }

        buffer_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (buffer_event_ == nullptr) {
            return make_error(ErrorCode::render_failed, "创建空间音频缓冲事件失败", "system-spatial");
        }

        SpatialAudioObjectRenderStreamActivationParams params{};
        params.ObjectFormat = &object_format_;
        params.StaticObjectTypeMask = windows_layouts::static_object_mask(*layout_);
        params.MinDynamicObjectCount = 0;
        params.MaxDynamicObjectCount = 0;
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

        objects_.assign(layout_->object_types.size(), nullptr);
        scratch_.assign(0, 0.0F);

        hr = render_stream_->Start();
        if (FAILED(hr)) {
            return hr_error(ErrorCode::render_failed, "启动空间音频流失败", "system-spatial", hr);
        }
        return {};
    }

    void pump() {
        while (!stop_.load(std::memory_order_acquire)) {
            const DWORD wait = WaitForSingleObject(buffer_event_, 100);
            if (stop_.load(std::memory_order_acquire)) {
                break;
            }
            if (wait != WAIT_OBJECT_0) {
                continue; // timeout: re-check stop_ and keep waiting
            }

            UINT32 available_dynamic = 0;
            UINT32 frame_count = 0;
            HRESULT hr = render_stream_->BeginUpdatingAudioObjects(&available_dynamic, &frame_count);
            if (FAILED(hr) || frame_count == 0) {
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

            // Deinterleave into each static object's mono buffer. Static objects persist for the
            // stream's lifetime, so activate each once (lazily) and reuse the ComPtr thereafter.
            for (std::size_t ch = 0; ch < objects_.size(); ++ch) {
                if (objects_[ch] == nullptr) {
                    hr = render_stream_->ActivateSpatialAudioObject(layout_->object_types[ch], &objects_[ch]);
                    if (FAILED(hr) || objects_[ch] == nullptr) {
                        objects_[ch] = nullptr;
                        continue;
                    }
                }
                BYTE* buffer = nullptr;
                UINT32 buffer_bytes = 0;
                hr = objects_[ch]->GetBuffer(&buffer, &buffer_bytes);
                if (FAILED(hr) || buffer == nullptr) {
                    continue;
                }
                const UINT32 want_bytes = frame_count * static_cast<UINT32>(sizeof(float));
                const UINT32 copy_frames =
                    (buffer_bytes < want_bytes ? buffer_bytes : want_bytes) / static_cast<UINT32>(sizeof(float));
                auto* dst = reinterpret_cast<float*>(buffer);
                for (UINT32 f = 0; f < copy_frames; ++f) {
                    dst[f] = scratch_[static_cast<std::size_t>(f) * channels_ + ch];
                }
            }

            render_stream_->EndUpdatingAudioObjects();
        }
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
