#include "apple_renderer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

#include "adm/render_apple.h"

#include "../../../src/adm_apple/apple_backend.h"
#include "render_common.h"

namespace mradm {
namespace {

struct ApplePreparedRender final : IPreparedRender {
    explicit ApplePreparedRender(std::shared_ptr<const apple::Prepared> p) : state(std::move(p)) {}
    std::shared_ptr<const apple::Prepared> state;
};

// Error buffers for one island call.
class IslandError {
  public:
    [[nodiscard]] const MradmEngineIslandError* get() noexcept {
        message_len_ = message_.size();
        context_len_ = context_.size();
        error_ = {message_.data(), &message_len_, context_.data(), &context_len_};
        return &error_;
    }
    [[nodiscard]] Error error(int32_t status) const {
        return Error{static_cast<ErrorCode>(status),
                     std::string{reinterpret_cast<const char*>(message_.data()), message_len_},
                     std::string{reinterpret_cast<const char*>(context_.data()), context_len_}};
    }

  private:
    std::array<uint8_t, 1024> message_{};
    std::array<uint8_t, 1024> context_{};
    std::size_t message_len_{0};
    std::size_t context_len_{0};
    MradmEngineIslandError error_{};
};

// The island table as the C++ stream interface.
class TableStream final : public IRenderStream {
  public:
    TableStream(MradmEngineIslandStream table, std::string layout) : t_(table), layout_(std::move(layout)) {}
    ~TableStream() override { t_.destroy(t_.ctx); }
    TableStream(const TableStream&) = delete;
    TableStream& operator=(const TableStream&) = delete;
    TableStream(TableStream&&) = delete;
    TableStream& operator=(TableStream&&) = delete;

    [[nodiscard]] Result<std::size_t> process(std::span<float> out, std::size_t frames) override {
        return produced(t_.process, out, frames);
    }
    [[nodiscard]] Result<void> seek(uint64_t frame) override { return done(t_.seek, frame); }
    [[nodiscard]] bool seek_requires_preroll() const override { return t_.requires_preroll != 0U; }
    [[nodiscard]] Result<void> seek_with_cancel(uint64_t frame, std::stop_token cancel) override {
        t_.cancel_begin(t_.ctx);
        const std::stop_callback on_stop(cancel, [this] { t_.cancel_request(t_.ctx); });
        return done(t_.seek_cancellable, frame);
    }
    [[nodiscard]] Result<void> validate_overrides(const LiveOverrides& overrides) const override {
        const auto records = render_common::live_override_records(overrides);
        IslandError e;
        const auto status = t_.validate(t_.ctx, records.data(), records.size(), overrides.revision, e.get());
        if (status != 0) {
            return mradm::unexpected{e.error(status)};
        }
        return {};
    }
    void set_overrides(const LiveOverrides& overrides) override {
        const auto records = render_common::live_override_records(overrides);
        t_.set_overrides(t_.ctx, records.data(), records.size(), overrides.revision);
    }
    void set_listener_orientation(const ListenerOrientation& o) override {
        t_.set_orientation(t_.ctx, o.yaw_deg, o.pitch_deg, o.roll_deg);
    }
    [[nodiscard]] bool renders_orientation_at_output() const override { return t_.renders_orientation != 0U; }
    [[nodiscard]] uint32_t intermediate_channels() const override { return t_.intermediate_channels; }
    [[nodiscard]] Result<std::size_t> produce_intermediate(std::span<float> out, std::size_t frames) override {
        return produced(t_.produce_intermediate, out, frames);
    }
    [[nodiscard]] std::size_t render_output(std::span<const float> intermediate,
                                            std::span<float> out,
                                            std::size_t frames,
                                            const ListenerOrientation& o) override {
        return t_.render_output(t_.ctx,
                                intermediate.data(),
                                intermediate.size(),
                                out.data(),
                                out.size(),
                                frames,
                                o.yaw_deg,
                                o.pitch_deg,
                                o.roll_deg);
    }
    [[nodiscard]] bool render_failed() const override { return t_.render_failed(t_.ctx) != 0U; }
    [[nodiscard]] Result<void> reposition_source(uint64_t frame) override { return done(t_.reposition_source, frame); }
    void loop_render_reset(uint64_t frame) override { t_.loop_render_reset(t_.ctx, frame); }
    [[nodiscard]] uint32_t out_channels() const override { return t_.out_channels; }
    [[nodiscard]] uint32_t sample_rate() const override { return t_.sample_rate; }
    // The requested layout (the table does not carry the container's tag).
    [[nodiscard]] std::string_view output_layout() const override { return layout_; }

  private:
    using Produce = int32_t (*)(void*, float*, size_t, size_t, size_t*, const MradmEngineIslandError*);
    using Frame = int32_t (*)(void*, uint64_t, const MradmEngineIslandError*);

    [[nodiscard]] Result<std::size_t> produced(Produce f, std::span<float> out, std::size_t frames) {
        IslandError e;
        std::size_t n = 0;
        const auto status = f(t_.ctx, out.data(), out.size(), frames, &n, e.get());
        if (status != 0) {
            return mradm::unexpected{e.error(status)};
        }
        return n;
    }
    [[nodiscard]] Result<void> done(Frame f, uint64_t frame) {
        IslandError e;
        const auto status = f(t_.ctx, frame, e.get());
        if (status != 0) {
            return mradm::unexpected{e.error(status)};
        }
        return {};
    }

    MradmEngineIslandStream t_;
    std::string layout_;
};

class AppleRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override { return apple_capabilities(); }

    [[nodiscard]] Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override {
        auto prepared = apple::prepare(plan, logs);
        if (!prepared) {
            return mradm::unexpected{prepared.error()};
        }
        return std::shared_ptr<IPreparedRender>{std::make_shared<ApplePreparedRender>(std::move(*prepared))};
    }

    [[nodiscard]] Result<RenderMetrics>
    render_window(const IPreparedRender& prep, const RenderPlan& plan, ProgressSink& progress, LogSink& logs) override {
        const auto* prepared = dynamic_cast<const ApplePreparedRender*>(&prep);
        if (prepared == nullptr) {
            return make_error(
                ErrorCode::internal_error, "apple: render_window received an incompatible prepared state", {});
        }
        return apple::render_window(*prepared->state, plan, progress, logs);
    }

    [[nodiscard]] Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& prep, const RenderPlan& plan, LogSink& logs) override {
        const auto* prepared = dynamic_cast<const ApplePreparedRender*>(&prep);
        if (prepared == nullptr) {
            return make_error(
                ErrorCode::internal_error, "apple: open_stream received an incompatible prepared state", {});
        }
        auto table = apple::open_stream(*prepared->state, plan, logs);
        if (!table) {
            return mradm::unexpected{table.error()};
        }
        return std::unique_ptr<IRenderStream>{std::make_unique<TableStream>(*table, plan.output_layout)};
    }
};

} // namespace

std::unique_ptr<IRenderer> create_apple_renderer() {
    return std::make_unique<AppleRenderer>();
}

} // namespace mradm
