#include <algorithm>
#include <mutex>
#include <utility>

#include "room_internal.h"

namespace mradm {
namespace {
std::string utf8_path(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
Result<void> checked(int status, const char* message = "房间 DSP 操作失败") {
    if (status != 0) {
        return make_error(static_cast<ErrorCode>(status), message, "room-binaural");
    }
    return {};
}
} // namespace

Result<std::shared_ptr<const RoomProfile>> RoomProfile::load(const std::filesystem::path& manifest) {
    const auto path = utf8_path(manifest);
    std::array<char, 512> message{};
    void* raw = nullptr;
    auto result = checked(mradm_room_profile_load(path.data(), path.size(), &raw, message.data(), message.size()),
                          message.data());
    if (!result) {
        return tl::unexpected{result.error()};
    }
    std::unique_ptr<void, decltype(&mradm_room_profile_destroy)> owned{raw, mradm_room_profile_destroy};
    return std::shared_ptr<const RoomProfile>{new RoomProfile(owned.release())};
}
RoomProfile::~RoomProfile() {
    mradm_room_profile_destroy(handle_);
}
std::string RoomProfile::manifest_json() const {
    return mradm_room_profile_json(handle_);
}

Result<std::string> inspect_room(const RoomConfig& config) {
    auto result = room_internal::prepare(config);
    if (!result) {
        return tl::unexpected{result.error()};
    }
    return (*result)->json;
}

namespace room_internal {
MradmRoomScreenGeometry geometry(const ScreenGeometry& screen) {
    MradmRoomScreenGeometry out{};
    out.cartesian = screen.cartesian ? 1U : 0U;
    out.aspect_ratio = screen.aspect_ratio;
    std::ranges::copy(screen.centre, std::begin(out.centre));
    out.width = screen.width;
    return out;
}
MradmRoomScreens screens(const ScreenConfig& config, const ScreenGeometry& reference) {
    MradmRoomScreens result{};
    result.enabled = config.reproduction.has_value() ? 1U : 0U;
    result.reference = geometry(reference);
    result.reproduction = geometry(config.reproduction.value_or(reference));
    return result;
}
Result<std::shared_ptr<const Prepared>>
prepare(const RoomConfig& config, const std::filesystem::path& sofa, const Prepared* reuse) {
    if (!config.profile) {
        return make_error(ErrorCode::invalid_argument, "room-binaural 需要房间包");
    }
    const auto hrir = utf8_path(config.hrir_manifest);
    const auto sofa_path = utf8_path(sofa);
    std::array<char, 512> message{};
    void* raw = nullptr;
    auto status = checked(mradm_room_prepare_quality(Access::get(*config.profile),
                                                     hrir.data(),
                                                     hrir.size(),
                                                     sofa_path.data(),
                                                     sofa_path.size(),
                                                     reuse != nullptr ? reuse->handle.get() : nullptr,
                                                     static_cast<std::uint32_t>(config.quality),
                                                     config.metres_per_unit,
                                                     &raw,
                                                     message.data(),
                                                     message.size()),
                          message.data());
    if (!status) {
        return tl::unexpected{status.error()};
    }
    PreparedHandle owned{raw, mradm_room_prepared_destroy};
    auto result = std::make_shared<Prepared>();
    result->handle = std::move(owned);
    auto info = checked(mradm_room_prepared_info(raw, &result->info));
    if (!info) {
        return tl::unexpected{info.error()};
    }
    result->json = mradm_room_prepared_json(raw);
    return std::shared_ptr<const Prepared>{std::move(result)};
}

ConfigRecord::ConfigRecord(const RoomConfig& config) : hrir(utf8_path(config.hrir_manifest)) {
    record.profile = config.profile ? Access::get(*config.profile) : nullptr;
    record.hrir = {reinterpret_cast<const std::uint8_t*>(hrir.data()), hrir.size()};
    record.metres = config.metres_per_unit;
    record.quality = static_cast<std::uint32_t>(config.quality);
}

namespace {
// Switch requests, the preparation thread and the status snapshots are Rust mradm-render
// `room_runtime` (PCM consistency 4.6 E); this shell converts the records.
class RoomControl final : public IRoomControl {
  public:
    explicit RoomControl(MradmRenderRoomControl* raw) : handle_(raw, &mradm_room_control_destroy) {}
    [[nodiscard]] Result<std::uint64_t> request(const RoomConfig& config) override {
        const ConfigRecord record(config);
        std::array<std::uint8_t, 512> message{};
        std::array<std::uint8_t, 512> context{};
        auto message_len = message.size();
        auto context_len = context.size();
        std::uint64_t revision = 0;
        if (const auto code = mradm_room_control_request(
                handle_.get(), &record.record, &revision, message.data(), &message_len, context.data(), &context_len);
            code != 0) {
            return make_error(static_cast<ErrorCode>(code),
                              std::string{reinterpret_cast<const char*>(message.data()), message_len},
                              std::string{reinterpret_cast<const char*>(context.data()), context_len});
        }
        return revision;
    }
    [[nodiscard]] RoomStatus status() const override {
        const std::lock_guard lock(mutex_); // the record's message lives until the next status call
        MradmRenderRoomStatus record{};
        mradm_room_control_status(handle_.get(), &record);
        RoomStatus result;
        result.state = static_cast<RoomSwitchState>(record.state);
        result.requested_revision = record.requested_revision;
        result.applied_revision = record.applied_revision;
        result.transition_sample = record.transition_sample;
        result.material_frames = record.material_frames;
        result.tail_frames = record.tail_frames;
        result.buffer_frames = record.buffer_frames;
        result.material_filter_guard_frames = record.material_filter_guard_frames;
        result.constrained_sources = record.constrained_sources;
        result.message.assign(reinterpret_cast<const char*>(record.message.data), record.message.len);
        result.quality = static_cast<RoomQuality>(record.quality);
        result.reflection_update_frames = record.reflection_update_frames;
        return result;
    }
    [[nodiscard]] RoomSourcePosition source_position(std::uint64_t id) const override {
        MradmRoomSourceInfo source{};
        mradm_room_control_source(handle_.get(), id, &source);
        RoomSourcePosition result;
        result.element_id = source.id;
        std::ranges::copy(source.position, result.position_metres.begin());
        std::ranges::copy(source.direction, result.direction.begin());
        result.distance_metres = source.distance;
        result.constrained = source.constrained != 0;
        result.present = source.present != 0;
        return result;
    }

  private:
    std::unique_ptr<MradmRenderRoomControl, decltype(&mradm_room_control_destroy)> handle_;
    mutable std::mutex mutex_;
};
} // namespace

std::shared_ptr<IRoomControl> control(MradmRenderRoomControl* raw) {
    if (raw == nullptr) {
        return nullptr;
    }
    return std::make_shared<RoomControl>(raw);
}
} // namespace room_internal
} // namespace mradm
