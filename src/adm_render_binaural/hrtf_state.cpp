#include "hrtf_state.h"

#include <utility>

#include "live_rust_renderer.h"

namespace mradm::binaural_internal {
namespace {
std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {bytes.begin(), bytes.end()};
}
MradmAdmText text(const std::string& value) {
    return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}
struct Request {
    std::uint32_t sofa;
    MradmAdmText path;
    MradmAdmText display;
    MradmAdmText file_name;
    MradmBinauralHrtfHandle** output;
    MradmRenderBinauralHrtfView* view;
};
template <typename Call>
Result<std::shared_ptr<const HrtfState>> adopt(const std::filesystem::path* sofa, const Call& call) {
    const auto path = sofa != nullptr ? utf8(*sofa) : std::string{};
    const auto display = sofa != nullptr ? sofa->string() : std::string{};
    const auto file_name = sofa != nullptr ? sofa->filename().string() : std::string{};
    MradmBinauralHrtfHandle* raw = nullptr;
    MradmRenderBinauralHrtfView view{};
    live_scene::RustError error(0, display.size());
    const Request request{sofa != nullptr ? 1U : 0U, text(path), text(display), text(file_name), &raw, &view};
    if (const auto code = call(request, error); code != 0) {
        return error.error(code);
    }
    auto state = std::make_shared<HrtfState>();
    state->handle.reset(raw, mradm_render_binaural_hrtf_destroy);
    auto filters = dsp::HrtfFilters::adopt(view.filters);
    if (!filters) {
        return tl::unexpected{filters.error()};
    }
    state->filters = std::move(*filters);
    state->dataset_name.assign(reinterpret_cast<const char*>(view.name.data), view.name.len);
    state->sample_rate = static_cast<int>(view.sample_rate);
    state->num_dirs = static_cast<int>(view.num_dirs);
    state->hrir_len = static_cast<int>(view.hrir_len);
    state->fft_size = static_cast<int>(view.fft_size);
    state->n_bands = (state->fft_size / 2) + 1;
    state->overlap_len = static_cast<int>(view.overlap);
    state->hrtf_td = {view.hrirs, view.hrirs_len};
    state->grid_dirs_deg = {view.directions, view.directions_len};
    return std::shared_ptr<const HrtfState>{std::move(state)};
}
} // namespace

Result<std::shared_ptr<const HrtfState>>
HrtfState::prepare(const std::optional<std::filesystem::path>& sofa, std::uint32_t input_rate, std::uint64_t block) {
    return adopt(sofa ? &*sofa : nullptr, [&](const Request& r, live_scene::RustError& error) {
        return mradm_render_binaural_hrtf_prepare(r.sofa,
                                                  r.path,
                                                  r.display,
                                                  r.file_name,
                                                  input_rate,
                                                  block,
                                                  r.output,
                                                  r.view,
                                                  error.message(),
                                                  error.message_len(),
                                                  error.context(),
                                                  error.context_len());
    });
}

Result<std::shared_ptr<const HrtfState>> HrtfState::prepare_live(const std::filesystem::path& sofa,
                                                                 std::uint32_t sample_rate) {
    return adopt(sofa.empty() ? nullptr : &sofa, [&](const Request& r, live_scene::RustError& error) {
        return mradm_render_binaural_hrtf_live(r.sofa,
                                               r.path,
                                               r.display,
                                               r.file_name,
                                               sample_rate,
                                               r.output,
                                               r.view,
                                               error.message(),
                                               error.message_len(),
                                               error.context(),
                                               error.context_len());
    });
}

} // namespace mradm::binaural_internal
