#include "size_processor.h"

#include <algorithm>
#include <iterator>
#include <limits>
namespace mradm::triple_balance {
SizeDecorrelator::SizeDecorrelator() {
    void* raw = nullptr;
    dsp::tb_checked(mradm_dsp_tb_filter_create, &raw);
    handle_.reset(raw);
}
void SizeDecorrelator::reset() noexcept {
    dsp::tb_checked(mradm_dsp_tb_filter_reset, handle_.get());
}
void SizeDecorrelator::process(std::span<const float, 32> input, std::span<FilteredFrame, 32> output) noexcept {
    dsp::tb_checked(
        mradm_dsp_tb_filter_process, handle_.get(), input.data(), input.size(), output.front().data(), output.size());
}
Result<SizeObjectProcessor>
SizeObjectProcessor::create(std::span<const SizeEvent> events, const std::string& layout, uint32_t rate) {
    auto code = dsp::tb_layout(layout);
    if (!code) {
        return tl::unexpected{code.error()};
    }
    std::vector<MradmTbEvent> transfer;
    transfer.reserve(events.size());
    std::ranges::transform(events, std::back_inserter(transfer), [](const auto& e) {
        return MradmTbEvent{e.start_sample, {e.position.x, e.position.y, e.position.z}, e.size};
    });
    void* raw = nullptr;
    auto status = dsp::tb_status(mradm_dsp_tb_object_create, transfer.data(), transfer.size(), *code, rate, &raw);
    if (!status) {
        return tl::unexpected{status.error()};
    }
    SizeObjectProcessor result;
    result.handle_.reset(raw);
    result.channels_ = dsp::tb_channels(*code);
    return result;
}
void SizeObjectProcessor::reset() noexcept {
    dsp::tb_checked(mradm_dsp_tb_object_reset, handle_.get());
}
void SizeObjectProcessor::set_size_scale(float scale) noexcept {
    dsp::tb_checked(mradm_dsp_tb_object_scale, handle_.get(), scale);
}
Result<void> SizeObjectProcessor::append(std::span<const float> input, std::vector<float>& output, bool final_block) {
    std::size_t samples = 0;
    auto status =
        dsp::tb_status(mradm_dsp_tb_object_required, handle_.get(), input.size(), final_block ? 1U : 0U, &samples);
    if (!status) {
        return status;
    }
    if (samples > output.max_size() - output.size()) {
        return make_error(ErrorCode::invalid_argument, "Size output overflow");
    }
    const auto before = output.size();
    output.resize(before + samples);
    std::size_t frames = 0;
    auto tail = std::span{output}.subspan(before);
    status = dsp::tb_status(mradm_dsp_tb_object_process,
                            handle_.get(),
                            input.data(),
                            input.size(),
                            tail.data(),
                            tail.size(),
                            final_block ? 1U : 0U,
                            &frames);
    if (!status) {
        output.resize(before);
        return status;
    }
    if (frames * channels_ != samples) {
        std::terminate();
    }
    return {};
}
Result<void> SizeObjectProcessor::push(std::span<const float> input, std::vector<float>& output) {
    return append(input, output, false);
}
Result<void> SizeObjectProcessor::finish(std::vector<float>& output) {
    return append({}, output, true);
}
MradmTbObjectStatus SizeObjectProcessor::state_info() const {
    MradmTbObjectStatus result{};
    dsp::tb_checked(mradm_dsp_tb_object_status, handle_.get(), &result);
    return result;
}
SizeProcessorState SizeObjectProcessor::snapshot() const {
    void* raw = nullptr;
    dsp::tb_checked(mradm_dsp_tb_object_snapshot_create, handle_.get(), &raw);
    return SizeProcessorState{raw, mradm_dsp_tb_object_snapshot_destroy};
}
void SizeObjectProcessor::restore(const SizeProcessorState& state) noexcept {
    dsp::tb_checked(mradm_dsp_tb_object_snapshot_restore, handle_.get(), state.get());
}
} // namespace mradm::triple_balance
