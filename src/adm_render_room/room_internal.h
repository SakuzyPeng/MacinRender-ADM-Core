#pragma once
#include <array>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include "adm/options.h"
#include "adm/room.h"

#include "room_ffi.h"

namespace mradm::room_internal {
struct Access {
    static const void* get(const RoomProfile& profile) { return profile.handle_; }
};
using PreparedHandle = std::unique_ptr<void, decltype(&mradm_room_prepared_destroy)>;
using InstanceHandle = std::unique_ptr<void, decltype(&mradm_room_instance_destroy)>;
// Members are used by the out-of-line implementation in room.cpp.
// cppcheck-suppress-begin unusedStructMember
struct Prepared {
    PreparedHandle handle{nullptr, mradm_room_prepared_destroy};
    MradmRoomPreparedInfo info{};
    std::string json;
};
[[nodiscard]] Result<std::shared_ptr<const Prepared>>
prepare(const RoomConfig& config, const std::filesystem::path& sofa = {}, const Prepared* reuse = nullptr);
[[nodiscard]] Result<std::array<float, 3>> polar(float azimuth, float elevation, float distance);

// A render-worker session plus a separate preparation/disposal thread. Neither
// device callback owns this object nor accesses its locks or Rust session.
class Runtime final : public IRoomControl {
  public:
    static Result<std::shared_ptr<Runtime>>
    create(std::shared_ptr<const Prepared> prepared, const RoomConfig& config, bool scene, bool spread);
    ~Runtime() override;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;
    [[nodiscard]] Result<void> configure(std::span<const MradmRoomDescription> descriptions);
    [[nodiscard]] Result<void> reset(std::uint64_t sample = 0, bool clear = false);
    [[nodiscard]] Result<void> prime(const MradmRoomCommand& command, std::uint64_t sample);
    [[nodiscard]] Result<void> pose(const ListenerOrientation& orientation);
    [[nodiscard]] Result<void> overrides(std::span<const MradmRoomOverride> values);
    [[nodiscard]] Result<std::uint64_t> end_input();
    [[nodiscard]] Result<void> process(std::size_t frames,
                                       std::span<const MradmRoomPlane> planes,
                                       std::span<const MradmRoomCommand> initial,
                                       std::span<const MradmRoomCommand> events,
                                       std::span<float> out);
    [[nodiscard]] Result<std::uint64_t> request(const RoomConfig& config) override;
    [[nodiscard]] RoomStatus status() const override;
    [[nodiscard]] RoomSourcePosition source_position(std::uint64_t id) const override;
    [[nodiscard]] std::uint64_t preroll_frames() const;
    [[nodiscard]] std::uint64_t tail_frames() const;

  private:
    Runtime(std::shared_ptr<const Prepared> prepared, RoomConfig config, void* handle);
    void prepare_loop(const std::stop_token& stop);
    [[nodiscard]] Result<void> pump();
    void publish(const MradmRoomInfo& info);
    struct Request {
        RoomConfig config;
        std::uint64_t revision{0};
        std::uint64_t epoch{0};
    };
    struct Candidate {
        InstanceHandle instance{nullptr, mradm_room_instance_destroy};
        std::uint64_t revision{0};
        std::uint64_t epoch{0};
        std::uint64_t preroll{0};
        std::size_t capacity{0};
    };
    std::shared_ptr<const Prepared> prepared_;
    RoomConfig config_;
    std::unique_ptr<void, decltype(&mradm_room_session_destroy)> session_;
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    RoomStatus status_;
    std::optional<Request> request_;
    std::optional<Candidate> candidate_;
    std::array<void*, 8> dispose_{};
    std::size_t dispose_count_{0};
    std::size_t capacity_{0};
    std::uint64_t epoch_{0};
    std::uint64_t latest_{0};
    std::uint64_t preparing_{0};
    std::uint64_t failed_{0};
    std::uint64_t preroll_{0};
    bool ended_{false};
    std::vector<RoomSourcePosition> positions_;
    std::jthread preparer_; // destroyed first; session/resources outlive preparation
};
// cppcheck-suppress-end unusedStructMember
} // namespace mradm::room_internal
