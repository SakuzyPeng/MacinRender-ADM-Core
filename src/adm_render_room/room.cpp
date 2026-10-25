#include <algorithm>
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

Result<std::array<float, 3>> polar(float azimuth, float elevation, float distance) {
    std::array<float, 3> result{};
    auto status = checked(mradm_room_polar(azimuth, elevation, distance, result.data()));
    if (!status) {
        return tl::unexpected{status.error()};
    }
    return result;
}

Runtime::Runtime(std::shared_ptr<const Prepared> prepared, RoomConfig config, void* handle)
    : prepared_(std::move(prepared)), config_(std::move(config)), session_(handle, mradm_room_session_destroy),
      preroll_(prepared_->info.preroll_frames) {
    status_.quality = config_.quality;
    status_.reflection_update_frames = prepared_->info.reflection_update_frames;
    status_.tail_frames = prepared_->info.tail_frames;
    status_.material_filter_guard_frames = prepared_->info.material_guard_frames;
    preparer_ = std::jthread([this](const std::stop_token& stop) { prepare_loop(stop); });
}
Runtime::~Runtime() {
    preparer_.request_stop();
    cv_.notify_all();
    preparer_.join();
    std::for_each_n(dispose_.begin(), dispose_count_, mradm_room_instance_destroy);
}
Result<std::shared_ptr<Runtime>>
Runtime::create(std::shared_ptr<const Prepared> prepared, const RoomConfig& config, bool scene, bool spread) {
    std::array<char, 512> message{};
    void* raw = nullptr;
    auto result = checked(mradm_room_session_create(prepared->handle.get(),
                                                    config.metres_per_unit,
                                                    scene ? 1U : 0U,
                                                    spread ? 1U : 0U,
                                                    &raw,
                                                    message.data(),
                                                    message.size()),
                          message.data());
    if (!result) {
        return tl::unexpected{result.error()};
    }
    std::unique_ptr<void, decltype(&mradm_room_session_destroy)> owned{raw, mradm_room_session_destroy};
    return std::shared_ptr<Runtime>{new Runtime(std::move(prepared), config, owned.release())};
}
Result<void> Runtime::configure(std::span<const MradmRoomDescription> descriptions) {
    std::array<char, 512> message{};
    auto result = checked(mradm_room_session_configure(
                              session_.get(), descriptions.data(), descriptions.size(), message.data(), message.size()),
                          message.data());
    if (!result) {
        return result;
    }
    MradmRoomInfo info{};
    auto query = checked(mradm_room_session_info(session_.get(), &info));
    if (!query) {
        return query;
    }
    {
        const std::lock_guard lock(mutex_);
        positions_.clear();
        positions_.reserve(descriptions.size());
        for (const auto& description : descriptions) {
            RoomSourcePosition position;
            position.element_id = description.id;
            positions_.push_back(position);
        }
    }
    publish(info);
    return {};
}
Result<void> Runtime::reset(std::uint64_t sample, bool clear) {
    std::array<char, 512> message{};
    auto result =
        checked(mradm_room_session_reset(session_.get(), sample, clear ? 1U : 0U, message.data(), message.size()),
                message.data());
    if (!result) {
        return result;
    }
    {
        const std::lock_guard lock(mutex_);
        ++epoch_;
        request_.reset();
        preparing_ = 0;
        failed_ = 0;
        ended_ = false;
        status_.message.clear();
    }
    MradmRoomInfo info{};
    auto query = checked(mradm_room_session_info(session_.get(), &info));
    if (!query) {
        return query;
    }
    publish(info);
    cv_.notify_all();
    return {};
}
Result<void> Runtime::prime(const MradmRoomCommand& command, std::uint64_t sample) {
    std::array<char, 512> message{};
    return checked(mradm_room_session_prime(session_.get(), &command, sample, message.data(), message.size()),
                   message.data());
}
Result<void> Runtime::pose(const ListenerOrientation& orientation) {
    const std::array<float, 3> pose{orientation.yaw_deg, orientation.pitch_deg, orientation.roll_deg};
    return checked(mradm_room_session_pose(session_.get(), pose.data(), pose.size()));
}
Result<void> Runtime::overrides(std::span<const MradmRoomOverride> values) {
    std::array<char, 512> message{};
    return checked(
        mradm_room_session_overrides(session_.get(), values.data(), values.size(), message.data(), message.size()),
        message.data());
}
Result<std::uint64_t> Runtime::end_input() {
    auto result = checked(mradm_room_session_end(session_.get()));
    if (!result) {
        return tl::unexpected{result.error()};
    }
    {
        const std::lock_guard lock(mutex_);
        ended_ = true;
        ++epoch_;
        request_.reset();
        preparing_ = 0;
    }
    MradmRoomInfo info{};
    auto query = checked(mradm_room_session_info(session_.get(), &info));
    if (!query) {
        return tl::unexpected{query.error()};
    }
    publish(info);
    cv_.notify_all();
    return info.tail_frames;
}
Result<void> Runtime::process(std::size_t frames,
                              std::span<const MradmRoomPlane> planes,
                              std::span<const MradmRoomCommand> initial,
                              std::span<const MradmRoomCommand> events,
                              std::span<float> out) {
    auto pending = pump();
    if (!pending) {
        return pending;
    }
    std::array<char, 512> message{};
    MradmRoomInfo info{};
    auto result = checked(mradm_room_session_process(session_.get(),
                                                     frames,
                                                     planes.data(),
                                                     planes.size(),
                                                     initial.data(),
                                                     initial.size(),
                                                     events.data(),
                                                     events.size(),
                                                     out.data(),
                                                     out.size(),
                                                     &info,
                                                     message.data(),
                                                     message.size()),
                          message.data());
    if (!result) {
        return result;
    }
    publish(info);
    return {};
}
void Runtime::publish(const MradmRoomInfo& info) {
    const std::lock_guard lock(mutex_);
    if (info.source_capacity > capacity_) {
        cv_.notify_all();
    }
    capacity_ = info.source_capacity;
    status_.material_frames = info.input_frames;
    status_.buffer_frames = info.latency_frames;
    status_.tail_frames = info.tail_frames;
    status_.material_filter_guard_frames = info.material_guard_frames;
    status_.applied_revision = info.applied_request;
    status_.transition_sample = info.transition_sample;
    status_.constrained_sources = info.constrained_sources;
    switch (info.phase) {
    case 1:
        status_.state = RoomSwitchState::preparing;
        break;
    case 2:
        status_.state = RoomSwitchState::transitioning;
        break;
    case 3:
        status_.state = RoomSwitchState::draining;
        break;
    default:
        status_.state = RoomSwitchState::applied;
        break;
    }
    if (preparing_ == latest_ && preparing_ != 0) {
        status_.state = RoomSwitchState::preparing;
    }
    if (failed_ == latest_ && failed_ != 0) {
        status_.state = RoomSwitchState::failed;
    }
    for (auto& position : positions_) {
        MradmRoomSourceInfo source{};
        if (mradm_room_session_source(session_.get(), position.element_id, &source) == 0) {
            std::ranges::copy(source.position, position.position_metres.begin());
            std::ranges::copy(source.direction, position.direction.begin());
            position.distance_metres = source.distance;
            position.present = source.present != 0;
            position.constrained = source.constrained != 0;
        }
    }
}
Result<std::uint64_t> Runtime::request(const RoomConfig& config) {
    if (!config.profile || config.quality != config_.quality || config.metres_per_unit != config_.metres_per_unit ||
        (!config.hrir_manifest.empty() && config.hrir_manifest != config_.hrir_manifest)) {
        return make_error(ErrorCode::invalid_argument, "房间切换必须保持当前档位、HRIR 和坐标比例");
    }
    const std::lock_guard lock(mutex_);
    if (ended_) {
        return make_error(ErrorCode::invalid_argument, "节目已结束，不能再切换房间");
    }
    auto selected = config;
    selected.hrir_manifest.clear(); // pin the active listener bank, independent of package default
    request_ = Request{std::move(selected), ++latest_, epoch_};
    preparing_ = latest_;
    status_.requested_revision = latest_;
    status_.state = RoomSwitchState::preparing;
    status_.message.clear();
    cv_.notify_all();
    return latest_;
}
RoomStatus Runtime::status() const {
    const std::lock_guard lock(mutex_);
    return status_;
}
RoomSourcePosition Runtime::source_position(std::uint64_t id) const {
    const std::lock_guard lock(mutex_);
    const auto found = std::ranges::find(positions_, id, &RoomSourcePosition::element_id);
    return found == positions_.end() ? RoomSourcePosition{id} : *found;
}
std::uint64_t Runtime::preroll_frames() const {
    const std::lock_guard lock(mutex_);
    return preroll_;
}
std::uint64_t Runtime::tail_frames() const {
    const std::lock_guard lock(mutex_);
    return status_.tail_frames;
}
Result<void> Runtime::pump() {
    const std::lock_guard lock(mutex_);
    if (latest_ != 0) {
        auto notified = checked(mradm_room_session_request(session_.get(), latest_));
        if (!notified) {
            return notified;
        }
    }
    // A stalled preparer must never force a destructor onto the render worker.
    // Request invalidation above still prevents an obsolete pending room starting.
    if (dispose_count_ + 3 > dispose_.size()) {
        return {};
    }
    void* retired = nullptr;
    if (mradm_room_session_retired(session_.get(), &retired) == 0 && retired != nullptr) {
        dispose_.at(dispose_count_++) = retired;
    }
    if (candidate_ && candidate_->epoch == epoch_ && candidate_->revision == latest_ &&
        candidate_->capacity >= capacity_ && !ended_) {
        std::array<void*, 2> displaced{};
        std::array<char, 512> message{};
        const auto code = mradm_room_session_queue(session_.get(),
                                                   candidate_->instance.release(),
                                                   candidate_->revision,
                                                   displaced.data(),
                                                   displaced.size(),
                                                   message.data(),
                                                   message.size());
        if (code != 0) {
            failed_ = latest_;
            status_.message = message.data();
        } else {
            preroll_ = std::max(preroll_, candidate_->preroll);
        }
        for (auto* item : displaced) {
            if (item != nullptr) {
                dispose_.at(dispose_count_++) = item;
            }
        }
        preparing_ = 0;
        candidate_.reset();
    }
    cv_.notify_all();
    return {};
}
void Runtime::prepare_loop(const std::stop_token& stop) {
    while (!stop.stop_requested()) {
        std::optional<Request> queued;
        std::optional<Candidate> growing;
        bool resize_candidate = false;
        std::size_t capacity = 0;
        std::array<void*, 8> dispose{};
        std::size_t count = 0;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, stop, [&] {
                return request_.has_value() || dispose_count_ != 0 ||
                       (candidate_ && (candidate_->epoch != epoch_ || candidate_->revision != latest_ ||
                                       candidate_->capacity < capacity_));
            });
            if (stop.stop_requested()) {
                return;
            }
            queued = std::exchange(request_, std::nullopt);
            capacity = capacity_;
            count = std::exchange(dispose_count_, 0);
            std::copy_n(dispose_.begin(), count, dispose.begin());
            if (candidate_ &&
                (candidate_->epoch != epoch_ || candidate_->revision != latest_ || candidate_->capacity < capacity)) {
                resize_candidate = candidate_->epoch == epoch_ && candidate_->revision == latest_;
                growing = std::exchange(candidate_, std::nullopt);
            }
        }
        for (std::size_t i = 0; i < count; ++i) {
            mradm_room_instance_destroy(dispose.at(i));
        }
        if (!queued && !resize_candidate) {
            continue;
        }
        Candidate candidate;
        std::array<char, 512> message{};
        Result<void> result;
        if (queued) {
            growing.reset(); // stale candidate disposal stays outside the worker's mutex
            auto prepared = prepare(queued->config, {}, prepared_.get());
            void* raw = nullptr;
            result = prepared ? checked(mradm_room_instance_create(
                                            (*prepared)->handle.get(), capacity, &raw, message.data(), message.size()),
                                        message.data())
                              : Result<void>{tl::unexpected{prepared.error()}};
            candidate = Candidate{InstanceHandle{raw, mradm_room_instance_destroy},
                                  queued->revision,
                                  queued->epoch,
                                  prepared ? (*prepared)->info.preroll_frames : 0,
                                  capacity};
        } else if (growing) {
            candidate = std::move(*growing);
            result =
                checked(mradm_room_instance_reserve(candidate.instance.get(), capacity, message.data(), message.size()),
                        message.data());
            candidate.capacity = capacity;
        } else {
            continue;
        }
        const std::lock_guard lock(mutex_);
        if (candidate.epoch != epoch_ || candidate.revision != latest_) {
            continue;
        }
        if (!result) {
            failed_ = latest_;
            preparing_ = 0;
            status_.state = RoomSwitchState::failed;
            status_.message = result.error().message;
        } else {
            candidate_ = std::move(candidate);
        }
    }
}
} // namespace room_internal
} // namespace mradm
