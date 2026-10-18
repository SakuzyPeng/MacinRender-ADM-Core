#include "live_room_renderer.h"

#include <algorithm>
#include <limits>
#include <unordered_map>

#include "render_common.h"
#include "room_internal.h"

namespace mradm::live_scene {
namespace {
MradmLiveBinauralState convert(const ObjectState& state) {
    MradmLiveBinauralState result{};
    result.valid = state.valid_fields;
    result.active = state.active ? 1U : 0U;
    result.channel_lock = state.channel_lock ? 1U : 0U;
    result.screen_reference = state.screen_reference ? 1U : 0U;
    result.head_locked = state.head_locked ? 1U : 0U;
    result.gain = state.linear_gain;
    result.position[0] = state.x;
    result.position[1] = state.y;
    result.position[2] = state.z;
    result.extent[0] = state.width;
    result.extent[1] = state.height;
    result.extent[2] = state.depth;
    result.diffuse = state.diffuse;
    result.divergence = state.divergence;
    result.divergence_range[0] = state.divergence_azimuth_range;
    result.divergence_range[1] = state.divergence_position_range;
    result.has_max_distance = state.channel_lock_max_distance.has_value() ? 1U : 0U;
    result.max_distance = state.channel_lock_max_distance.value_or(0.0F);
    return result;
}

class LiveRoomRenderer final : public ILiveSceneRenderer {
  public:
    LiveRoomRenderer(std::shared_ptr<room_internal::Runtime> runtime,
                     DiagnosticSink diagnostics,
                     MradmRoomScreens screens)
        : runtime_(std::move(runtime)), diagnostics_(std::move(diagnostics)), screens_(screens) {
        initial_.reserve(512);
        events_.reserve(4096);
    }
    Result<void> configure_generation(std::uint64_t generation_id,
                                      std::span<const ElementDescriptor> elements) override {
        std::vector<MradmRoomDescription> descriptions;
        descriptions.reserve(elements.size());
        std::unordered_map<std::uint64_t, std::uint32_t> indices;
        for (const auto& element : elements) {
            MradmRoomDescription description{};
            description.id = element.element_id;
            description.generation = generation_id;
            description.screens = screens_;
            description.role = static_cast<std::uint32_t>(element.role);
            std::array<float, 3> position{element.x, element.y, element.z};
            if (!element.has_position && element.role == ElementRole::direct_speaker) {
                const auto nominal = render_common::direct_speaker_position_for_labels({element.speaker_label});
                if (nominal) {
                    auto direction = room_internal::polar(nominal->azimuth, nominal->elevation, 1.0F);
                    if (!direction) {
                        return tl::unexpected{direction.error()};
                    }
                    position = *direction;
                } else if (diagnostics_) {
                    diagnostics_({LogLevel::warning,
                                  DiagnosticCode::direct_speaker_fallback,
                                  0,
                                  generation_id,
                                  element.element_id,
                                  state_position,
                                  "未知扬声器标签，room-binaural 使用正前方"});
                    position = {0, 1, 0};
                }
            }
            std::ranges::copy(position, std::begin(description.position));
            indices.emplace(element.element_id, static_cast<std::uint32_t>(descriptions.size()));
            descriptions.push_back(description);
        }
        auto result = runtime_->configure(descriptions);
        if (!result) {
            return result;
        }
        generation_ = generation_id;
        indices_ = std::move(indices);
        planes_.resize(elements.size());
        ended_ = false;
        return {};
    }
    void reset() override {
        const auto result = runtime_->reset(0, true);
        error_ = result ? std::optional<Error>{} : result.error();
        indices_.clear();
        planes_.clear();
        ended_ = false;
    }
    Result<void> render(const Frame& frame, std::span<float> output) override {
        if (error_) {
            return tl::unexpected{*error_};
        }
        if (frame.generation_id != generation_) {
            return make_error(ErrorCode::invalid_argument, "room-binaural generation 不匹配");
        }
        std::ranges::fill(planes_, MradmRoomPlane{});
        initial_.clear();
        events_.clear();
        if (!ended_) {
            for (const auto& plane : frame.pcm) {
                const auto found = indices_.find(plane.element_id);
                if (found == indices_.end()) {
                    return make_error(ErrorCode::invalid_argument, "room-binaural 收到未知声源 PCM");
                }
                planes_[found->second] = {plane.samples.data(), plane.samples.size(), plane.has_signal ? 1U : 0U, 0};
            }
            for (const auto& initial : frame.initial_states) {
                warn_screen(frame, initial.state, initial.element_id);
                const auto found = indices_.find(initial.element_id);
                if (found == indices_.end()) {
                    return make_error(ErrorCode::invalid_argument, "room-binaural 收到未知声源状态");
                }
                MradmRoomCommand command{};
                command.element = found->second;
                command.state = convert(initial.state);
                initial_.push_back(command);
            }
            for (const auto& update : frame.updates) {
                warn_screen(frame, update.state, update.element_id);
                const auto found = indices_.find(update.element_id);
                if (found == indices_.end()) {
                    return make_error(ErrorCode::invalid_argument, "room-binaural 收到未知声源控制");
                }
                MradmRoomCommand command{};
                command.element = found->second;
                command.offset = update.offset_samples;
                command.duration = update.jump_position ? 0 : update.ramp_duration_samples;
                command.changed = update.changed_fields;
                command.cleared = update.cleared_fields;
                command.state = convert(update.state);
                events_.push_back(command);
            }
        }
        return runtime_->process(frame.duration_samples, planes_, initial_, events_, output);
    }
    void set_listener_orientation(const ListenerOrientation& orientation) override {
        auto result = runtime_->pose(orientation);
        if (!result) {
            error_ = result.error();
        }
    }
    [[nodiscard]] std::shared_ptr<IRoomControl> room_control() const override { return runtime_; }
    Result<std::uint64_t> end_input() override {
        ended_ = true;
        return runtime_->end_input();
    }
    [[nodiscard]] std::uint32_t tail_input_frames() const noexcept override {
        return static_cast<std::uint32_t>(runtime_->tail_frames());
    }
    [[nodiscard]] std::uint32_t output_channels() const noexcept override { return 2; }
    [[nodiscard]] std::uint32_t sample_rate() const noexcept override { return runtime_->sample_rate(); }

  private:
    void warn_screen(const Frame& frame, const ObjectState& state, std::uint64_t element) {
        if ((state.valid_fields & state_screen_reference) != 0 && state.screen_reference && screens_.enabled == 0 &&
            !warned_screen_ && diagnostics_) {
            diagnostics_({LogLevel::warning,
                          DiagnosticCode::semantic_degraded,
                          frame.epoch_id,
                          generation_,
                          element,
                          state_screen_reference,
                          "room-binaural 未配置重放屏幕，screenRef 保留原方向"});
            warned_screen_ = true;
        }
    }
    std::shared_ptr<room_internal::Runtime> runtime_;
    DiagnosticSink diagnostics_;
    MradmRoomScreens screens_;
    std::uint64_t generation_{0};
    std::unordered_map<std::uint64_t, std::uint32_t> indices_;
    std::vector<MradmRoomPlane> planes_;
    std::vector<MradmRoomCommand> initial_;
    std::vector<MradmRoomCommand> events_;
    std::optional<Error> error_;
    bool ended_{false};
    bool warned_screen_{false};
};
} // namespace
Result<std::unique_ptr<ILiveSceneRenderer>> create_live_room_renderer(const RendererConfig& config,
                                                                      DiagnosticSink diagnostics) {
    auto screen_valid = validate_screen_config(config.screens);
    if (!screen_valid) {
        return tl::unexpected{screen_valid.error()};
    }
    auto prepared = room_internal::prepare(config.room, config.sofa_path);
    if (!prepared) {
        return tl::unexpected{prepared.error()};
    }
    if (config.sample_rate != (*prepared)->info.sample_rate || config.output_layout != "binaural") {
        return make_error(ErrorCode::unsupported, "room-binaural 需要与房间包采样率一致的双耳输出");
    }
    if (config.binaural_spread_mode == BinauralSpreadMode::saf_spreader) {
        return make_error(ErrorCode::unsupported, "room-binaural 不支持 SAF covariance spreader");
    }
    auto runtime = room_internal::Runtime::create(
        *prepared, config.room, true, config.binaural_spread_mode != BinauralSpreadMode::none);
    if (!runtime) {
        return tl::unexpected{runtime.error()};
    }
    return std::unique_ptr<ILiveSceneRenderer>{new LiveRoomRenderer(
        *runtime,
        std::move(diagnostics),
        room_internal::screens(config.screens, config.screens.reference.value_or(ScreenGeometry{})))};
}
} // namespace mradm::live_scene
