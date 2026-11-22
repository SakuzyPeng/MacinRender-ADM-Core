// The Apple backend's islands driven as the engine drives them (R3 F3): the render scene imported
// through the test FFI, prepare / render / open_stream on mradm_apple_islands(), and the monitor
// stream table. Replaces the C++ IRenderer adapter of tests/support/apple_renderer. macOS only.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "adm/audio_io.h"

#include "../../src/adm_apple/apple_island.h"
#include "island_test_ffi.h"

namespace apple_test {

inline MradmAdmText view(std::string_view text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

inline std::string text(MradmAdmText value) {
    return value.len == 0U ? std::string{} : std::string{reinterpret_cast<const char*>(value.data), value.len};
}

// The caller side of an island call: never cancels, ignores progress, keeps the log lines.
struct Caller {
    std::vector<std::pair<uint32_t, std::string>> lines;

    [[nodiscard]] MradmIslandContext context() {
        return {this,
                [](void* /*user*/) -> uint32_t { return 0U; },
                [](void* /*user*/, const MradmEngineProgress* /*event*/) {},
                [](void* user, uint32_t level, MradmAdmText /*module*/, MradmAdmText message) {
                    static_cast<Caller*>(user)->lines.emplace_back(level, text(message));
                }};
    }
    // adm::LogLevel: debug 0, info 1, warning 2, error 3.
    [[nodiscard]] bool warned(std::string_view needle) const {
        return std::ranges::any_of(lines, [needle](const auto& line) {
            return line.first == 2U && line.second.find(needle) != std::string::npos;
        });
    }
};

// The engine's request fields for the backend (MradmAppleSettings), owning their texts. Layouts
// are the canonical ids the engine passes ("binaural", "4+7+0", "9+10+3", ...).
struct Settings {
    std::string input;
    std::string layout{"binaural"};
    uint32_t routing{0U}; // DirectSpeakersRoutingMode: automatic, label, position, matrix
    std::optional<MradmDirectMatrixView> matrix;
    bool split_power{false};
    uint32_t speaker_spread{0U}; // SpeakerSpreadMode: automatic, none, mdap
    uint32_t spatial_preset{0U}; // AppleSpatialPreset: off, headphone default, headphone movie
    bool speaker_rendering_flags{false};
    std::array<float, 3> orientation{}; // yaw, pitch, roll in degrees

    [[nodiscard]] MradmAppleSettings record() const {
        MradmAppleSettings s{};
        s.input = view(input);
        s.layout = view(layout);
        s.routing = routing;
        s.has_matrix = matrix.has_value() ? 1U : 0U;
        if (matrix) {
            s.matrix = *matrix;
        }
        s.split_power = split_power ? 1U : 0U;
        s.speaker_spread = speaker_spread;
        s.spatial_preset = spatial_preset;
        s.speaker_rendering_flags = speaker_rendering_flags ? 1U : 0U;
        s.orientation[0] = orientation[0];
        s.orientation[1] = orientation[1];
        s.orientation[2] = orientation[2];
        return s;
    }
};

// The render scene of an ADM file, or of a plain channel bed when `labels` are given; null on failure.
inline MradmSceneHandle* import_scene(const std::string& path, const std::vector<std::string>& labels = {}) {
    std::vector<MradmAdmText> texts;
    texts.reserve(labels.size());
    std::ranges::transform(labels, std::back_inserter(texts), [](const std::string& label) { return view(label); });
    MradmSceneHandle* scene = nullptr;
    uint32_t context = 0U;
    std::array<uint8_t, 1024> message{};
    if (mradm::rust().tests->scene_import(reinterpret_cast<const uint8_t*>(path.data()),
                                          path.size(),
                                          labels.empty() ? 0U : 1U,
                                          0U,
                                          {},
                                          texts.data(),
                                          texts.size(),
                                          &scene,
                                          &context,
                                          message.data(),
                                          message.size()) != 0) {
        return nullptr;
    }
    return scene;
}

// One island reply: the code (0 ok) and its message.
struct Reply {
    int32_t code{0};
    // Read by the smoke and matrix tests when an island call fails.
    // cppcheck-suppress unusedStructMember
    std::string message;
    [[nodiscard]] bool ok() const { return code == 0; }
};

inline Reply reply_of(int32_t status, const MradmEngineIslandReply& reply) {
    return status != 0 ? Reply{status, "rejected arguments"} : Reply{reply.code, text(reply.message)};
}

// A preparation of the Apple backend over an imported scene (which prepare takes either way).
class Prepared {
  public:
    Prepared(const Settings& settings, MradmSceneHandle* scene) {
        if (scene == nullptr) {
            reply = {1, "scene import failed"};
            return;
        }
        const auto record = settings.record();
        auto context = caller.context();
        MradmEngineIslandReply raw{};
        const auto status = mradm_apple_islands()->prepare(&record, scene, &context, &handle_, &raw);
        reply = reply_of(status, raw);
    }
    explicit Prepared(const Settings& settings, const std::vector<std::string>& labels = {})
        : Prepared(settings, import_scene(settings.input, labels)) {}
    ~Prepared() {
        if (handle_ != nullptr) {
            mradm_apple_islands()->destroy(handle_);
        }
    }
    Prepared(const Prepared&) = delete;
    Prepared& operator=(const Prepared&) = delete;
    Prepared(Prepared&&) = delete;
    Prepared& operator=(Prepared&&) = delete;

    [[nodiscard]] bool ok() const { return reply.ok() && handle_ != nullptr; }
    [[nodiscard]] const MradmApplePrepared* get() const { return handle_; }

    Reply reply;
    Caller caller;

  private:
    MradmApplePrepared* handle_{nullptr};
};

// Interleaved float samples of a rendered WAVE file.
struct Audio {
    uint32_t channels{0U};
    uint64_t frames{0U};
    std::vector<float> samples;
};

inline std::optional<Audio> read_wav(const std::filesystem::path& path) {
    auto reader = mradm::audio::RenderInputReader::open(path.string());
    if (!reader) {
        return std::nullopt;
    }
    const auto& info = (*reader)->info();
    Audio audio{info.channels, info.frames, {}};
    if (audio.frames > audio.samples.max_size() / audio.channels) {
        return std::nullopt;
    }
    audio.samples.resize(static_cast<std::size_t>(audio.frames) * audio.channels);
    if ((*reader)->read(audio.samples.data(), audio.frames) != audio.frames) {
        return std::nullopt;
    }
    return audio;
}

// The offline render of a preparation to `output` (a WAVE path), optionally over a frame window.
inline Reply render(const Prepared& prepared,
                    const Settings& settings,
                    const std::filesystem::path& output,
                    Caller& caller,
                    std::optional<std::pair<uint64_t, uint64_t>> window = std::nullopt) {
    const auto record = settings.record();
    const auto path = output.string();
    auto context = caller.context();
    MradmEngineIslandReply raw{};
    const auto status = mradm_apple_islands()->render(prepared.get(),
                                                      &record,
                                                      view(path),
                                                      window ? 1U : 0U,
                                                      window ? window->first : 0U,
                                                      window ? window->second : 0U,
                                                      &context,
                                                      &raw);
    return reply_of(status, raw);
}

// A live edit (LiveObjectOverride): whole-object when `speaker_label` is empty.
struct Override {
    std::string object_id;
    float gain_db{0.0F};
    bool mute{false};
    std::optional<bool> head_locked;
    std::string speaker_label;
};

// The error buffers of one stream call.
class StreamError {
  public:
    [[nodiscard]] const MradmEngineIslandError* get() {
        message_len_ = message_.size();
        context_len_ = context_.size();
        error_ = {message_.data(), &message_len_, context_.data(), &context_len_};
        return &error_;
    }
    [[nodiscard]] std::string message() const { return {reinterpret_cast<const char*>(message_.data()), message_len_}; }

  private:
    std::array<uint8_t, 1024> message_{};
    std::array<uint8_t, 1024> context_{};
    std::size_t message_len_{0U};
    std::size_t context_len_{0U};
    MradmEngineIslandError error_{};
};

// The monitor stream table of a preparation; it keeps the preparation alive.
class Stream {
  public:
    Stream(const Prepared& prepared, const Settings& settings) : caller_(std::make_unique<Caller>()) {
        const auto record = settings.record();
        auto context = caller_->context();
        StreamError error;
        status_ = mradm_apple_islands()->open_stream(prepared.get(), &record, &context, &t_, error.get());
        if (status_ != 0) {
            t_ = {};
        }
    }
    ~Stream() {
        if (t_.destroy != nullptr) {
            t_.destroy(t_.ctx);
        }
    }
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    Stream(Stream&&) = delete;
    Stream& operator=(Stream&&) = delete;

    [[nodiscard]] bool ok() const { return status_ == 0 && t_.process != nullptr; }
    [[nodiscard]] uint32_t out_channels() const { return t_.out_channels; }
    [[nodiscard]] uint32_t intermediate_channels() const { return t_.intermediate_channels; }
    [[nodiscard]] bool renders_orientation_at_output() const { return t_.renders_orientation != 0U; }

    // Frames produced, or nullopt on an error.
    [[nodiscard]] std::optional<std::size_t> process(std::vector<float>& out, std::size_t frames) {
        return produce(t_.process, out, frames);
    }
    [[nodiscard]] std::optional<std::size_t> produce_intermediate(std::vector<float>& out, std::size_t frames) {
        return produce(t_.produce_intermediate, out, frames);
    }
    [[nodiscard]] std::size_t render_output(const float* intermediate,
                                            std::size_t intermediate_len,
                                            std::vector<float>& out,
                                            std::size_t frames,
                                            std::array<float, 3> orientation = {}) {
        return t_.render_output(t_.ctx,
                                intermediate,
                                intermediate_len,
                                out.data(),
                                out.size(),
                                frames,
                                orientation[0],
                                orientation[1],
                                orientation[2]);
    }
    [[nodiscard]] bool seek(uint64_t frame) {
        StreamError error;
        return t_.seek(t_.ctx, frame, error.get()) == 0;
    }
    [[nodiscard]] bool reposition_source(uint64_t frame) {
        StreamError error;
        return t_.reposition_source(t_.ctx, frame, error.get()) == 0;
    }
    void loop_render_reset(uint64_t frame) { t_.loop_render_reset(t_.ctx, frame); }
    void set_orientation(float yaw, float pitch = 0.0F, float roll = 0.0F) {
        t_.set_orientation(t_.ctx, yaw, pitch, roll);
    }
    void set_overrides(const std::vector<Override>& overrides, uint64_t revision) {
        std::vector<MradmRenderLiveOverride> records;
        records.reserve(overrides.size());
        for (const auto& o : overrides) {
            const MradmRenderOverride base{view(o.object_id),
                                           view(o.speaker_label),
                                           o.mute ? 1U : 0U,
                                           o.gain_db,
                                           o.head_locked.has_value() ? 1U : 0U,
                                           o.head_locked.value_or(false) ? 1U : 0U};
            records.push_back({base, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F});
        }
        t_.set_overrides(t_.ctx, records.data(), records.size(), revision);
    }

    // The whole timeline pulled in the repeating `chunks` pattern, or nullopt on an error.
    [[nodiscard]] std::optional<std::vector<float>> pull_all(const std::vector<std::size_t>& chunks) {
        std::vector<float> output;
        std::vector<float> block;
        for (std::size_t index = 0;; ++index) {
            const auto frames = chunks[index % chunks.size()];
            block.assign(frames * t_.out_channels, 0.0F);
            const auto produced = process(block, frames);
            if (!produced) {
                return std::nullopt;
            }
            if (*produced == 0U) {
                return output;
            }
            output.insert(
                output.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(*produced * t_.out_channels));
        }
    }

  private:
    using Produce = int32_t (*)(void*, float*, size_t, size_t, size_t*, const MradmEngineIslandError*);
    [[nodiscard]] std::optional<std::size_t> produce(Produce f, std::vector<float>& out, std::size_t frames) {
        StreamError error;
        std::size_t produced = 0U;
        if (f(t_.ctx, out.data(), out.size(), frames, &produced, error.get()) != 0) {
            return std::nullopt;
        }
        return produced;
    }

    std::unique_ptr<Caller> caller_;
    MradmEngineIslandStream t_{};
    int32_t status_{1};
};

// A whole stream of a fresh preparation-held stream, or nullopt when it does not open.
inline std::optional<std::vector<float>> stream_all(const Prepared& prepared,
                                                    const Settings& settings,
                                                    const std::vector<std::size_t>& chunks,
                                                    const std::vector<Override>& overrides = {}) {
    Stream stream(prepared, settings);
    if (!stream.ok()) {
        return std::nullopt;
    }
    if (!overrides.empty()) {
        stream.set_overrides(overrides, 1U);
    }
    return stream.pull_all(chunks);
}

} // namespace apple_test
