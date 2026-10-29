#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../adm_audio/wav_backend.h"
#include "render_common.h"
#include "render_state.h"

namespace mradm::triple_balance {
namespace {
using render_common::DriverError;

// Block rendering, live edits, checkpoints and the seek steps are Rust mradm-render
// `triple_balance_stream` (PCM consistency 4.6); this side owns cancellation.
class TripleBalanceStream final : public IRenderStream {
  public:
    TripleBalanceStream(MradmRenderTripleBalanceStream* raw, const Prepared& prepared, const RenderPlan& plan)
        : handle_(raw, &mradm_render_triple_balance_stream_destroy), input_path_(plan.input_path),
          layout_(plan.output_layout), outputs_(prepared.output_channels), sample_rate_(prepared.sample_rate) {}

    Result<std::size_t> process(std::span<float> out, std::size_t frames) override {
        DriverError error;
        std::size_t produced = 0;
        if (const auto code = mradm_render_triple_balance_stream_process(handle_.get(),
                                                                         out.data(),
                                                                         out.size(),
                                                                         frames,
                                                                         &produced,
                                                                         error.location(),
                                                                         error.message(),
                                                                         error.message_len());
            code != 0) {
            return error.error(code, input_path_);
        }
        return produced;
    }

    [[nodiscard]] Result<void> validate_overrides(const LiveOverrides& overrides) const override {
        const auto records = render_common::live_override_records(overrides);
        std::string message(512, '\0');
        std::string context(512, '\0');
        for (const auto& item : overrides.objects) {
            context.resize(std::max(context.size(), item.object_id.size() + 1));
        }
        auto message_len = message.size();
        auto context_len = context.size();
        if (const auto code = mradm_render_triple_balance_stream_validate(handle_.get(),
                                                                          records.data(),
                                                                          records.size(),
                                                                          reinterpret_cast<uint8_t*>(message.data()),
                                                                          &message_len,
                                                                          reinterpret_cast<uint8_t*>(context.data()),
                                                                          &context_len);
            code != 0) {
            message.resize(message_len);
            context.resize(context_len);
            return make_error(static_cast<ErrorCode>(code), std::move(message), std::move(context));
        }
        return {};
    }

    void set_overrides(const LiveOverrides& overrides) override {
        const auto records = render_common::live_override_records(overrides);
        if (mradm_render_triple_balance_stream_overrides(handle_.get(), records.data(), records.size()) != 0) {
            std::terminate();
        }
    }

    Result<void> seek(uint64_t frame) override { return seek_with_cancel(frame, {}); }
    [[nodiscard]] bool seek_requires_preroll() const override {
        return mradm_render_triple_balance_stream_requires_preroll(handle_.get()) != 0U;
    }

    Result<void> seek_with_cancel(uint64_t frame, std::stop_token cancel) override {
        if (cancel.stop_requested()) {
            return make_error(ErrorCode::cancelled, "Triple Balance seek cancelled");
        }
        DriverError error;
        uint32_t pending = 0;
        if (const auto code = mradm_render_triple_balance_stream_seek_begin(
                handle_.get(), frame, &pending, error.location(), error.message(), error.message_len());
            code != 0) {
            return error.error(code, input_path_);
        }
        while (pending != 0U) {
            if (cancel.stop_requested()) {
                return make_error(ErrorCode::cancelled, "Triple Balance seek cancelled");
            }
            if (const auto code = mradm_render_triple_balance_stream_seek_step(
                    handle_.get(), &pending, error.location(), error.message(), error.message_len());
                code != 0) {
                return error.error(code, input_path_);
            }
        }
        if (const auto code = mradm_render_triple_balance_stream_seek_finish(
                handle_.get(), error.location(), error.message(), error.message_len());
            code != 0) {
            return error.error(code, input_path_);
        }
        return {};
    }

    [[nodiscard]] uint32_t out_channels() const override { return outputs_; }
    [[nodiscard]] uint32_t sample_rate() const override { return sample_rate_; }
    [[nodiscard]] std::string_view output_layout() const override { return layout_; }

  private:
    std::unique_ptr<MradmRenderTripleBalanceStream, decltype(&mradm_render_triple_balance_stream_destroy)> handle_;
    std::string input_path_;
    std::string layout_;
    uint16_t outputs_;
    uint32_t sample_rate_;
};
} // namespace

Result<std::unique_ptr<IRenderStream>> open_stream(const Prepared& prepared, const RenderPlan& plan) {
    if (plan.scene.info.num_channels == 0 || prepared.output_channels == 0 ||
        prepared.sample_rate != plan.scene.info.sample_rate) {
        return make_error(ErrorCode::invalid_argument, "triple-balance: invalid stream format");
    }
    const auto path = audio::wav_path_utf8(plan.input_path);
    DriverError error;
    MradmRenderTripleBalanceStream* raw = nullptr;
    if (const auto code = mradm_render_triple_balance_stream_open(prepared.rust->handle.get(),
                                                                  reinterpret_cast<const uint8_t*>(path.data()),
                                                                  path.size(),
                                                                  plan.default_interp_ms,
                                                                  &raw,
                                                                  error.location(),
                                                                  error.message(),
                                                                  error.message_len());
        code != 0) {
        return error.error(code, plan.input_path);
    }
    return std::unique_ptr<IRenderStream>{std::make_unique<TripleBalanceStream>(raw, prepared, plan)};
}
} // namespace mradm::triple_balance
