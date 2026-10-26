// Test-only copy of the C++ HRTF preparation before PCM consistency 4.7 A (see binaural_internal.h).
#include <array>
#include <bit>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>

#include <fmt/format.h>

#include "binaural_internal.h"

namespace mradm::binaural_internal {

// Grid index into the pre-computed VBAP table (az_res=1°, el_res=1°).
int vbap_grid_idx(float az_deg, float el_deg) {
    std::size_t index = 0U;
    std::array<char, 256> message{};
    dsp::check(mradm_dsp_hrtf_grid_index(az_deg, el_deg, &index, message.data(), message.size()), message.data());
    return static_cast<int>(index);
}

// Both built-in and user HRTFs enter through the Rust-owned dataset boundary.
Result<HrtfDataset> read_rust_dataset(std::span<const std::uint8_t> bytes, bool builtin) {
    void* raw = nullptr;
    std::array<char, 256> error{};
    auto status =
        mradm_dsp_dataset_create(bytes.data(), bytes.size(), builtin ? 1 : 0, &raw, error.data(), error.size());
    if (status != 0) {
        return make_error(static_cast<ErrorCode>(status), error.data());
    }
    const std::unique_ptr<void, decltype(&mradm_dsp_dataset_destroy)> owner(raw, mradm_dsp_dataset_destroy);
    MradmDspDatasetInfo info{};
    status = mradm_dsp_dataset_info(raw, &info, error.data(), error.size());
    if (status != 0) {
        return make_error(static_cast<ErrorCode>(status), error.data());
    }
    if (info.num_dirs > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        info.ir_len > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return make_error(ErrorCode::unsupported, "HRTF dataset exceeds renderer dimensions");
    }
    HrtfDataset result;
    result.sample_rate = static_cast<int>(info.sample_rate);
    result.num_dirs = static_cast<int>(info.num_dirs);
    result.hrir_len = static_cast<int>(info.ir_len);
    result.dirs_deg.resize(info.num_dirs * 2U);
    result.hrirs.resize(info.num_dirs * 2U * info.ir_len);
    std::array<char, 256> name{};
    status = mradm_dsp_dataset_copy(raw,
                                    result.dirs_deg.data(),
                                    result.dirs_deg.size(),
                                    result.hrirs.data(),
                                    result.hrirs.size(),
                                    name.data(),
                                    name.size(),
                                    error.data(),
                                    error.size());
    if (status != 0) {
        return make_error(static_cast<ErrorCode>(status), error.data());
    }
    result.name = name.data();
    return result;
}

HrtfDataset built_in_kemar_dataset() {
    auto result = read_rust_dataset({}, true);
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    return std::move(*result);
}

Result<HrtfDataset> load_sofa_dataset(const std::filesystem::path& path, std::uint32_t input_sample_rate) {
#if MR_ADM_ENABLE_SOFA
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return make_error(ErrorCode::io_error, "SOFA file could not be opened", "path=" + path.string());
    }
    const std::streamoff length = input.tellg();
    if (length <= 0) {
        return make_error(ErrorCode::io_error, "SOFA file is empty or unreadable");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length))) {
        return make_error(ErrorCode::io_error, "SOFA file could not be read completely");
    }
    auto result = read_rust_dataset(bytes, false);
    if (!result) {
        return result;
    }
    if (input_sample_rate != 0U && !std::cmp_equal(input_sample_rate, result->sample_rate)) {
        return make_error(
            ErrorCode::unsupported,
            fmt::format("SOFA: sample rate {} Hz does not match input {} Hz", result->sample_rate, input_sample_rate),
            "path=" + path.string());
    }
    result->name = fmt::format("SOFA {}", result->name.empty() ? path.filename().string() : result->name);
    return result;
#else
    (void) input_sample_rate;
    return make_error(ErrorCode::unsupported,
                      "SOFA loading is disabled in this build (MR_ADM_ENABLE_SOFA=OFF)",
                      "path=" + path.string());
#endif
}

// Preparation publishes only fully validated, immutable Rust interpolation data.
Result<std::unique_ptr<BinauralState>>
build_binaural_state(HrtfDataset dataset, uint64_t block_size, bool cache_magnitudes) {
    constexpr std::size_t k_max_fft = std::size_t{1U} << 24U;
    if (dataset.num_dirs <= 0 || dataset.hrir_len <= 0 || block_size == 0U || block_size > k_max_fft ||
        static_cast<std::size_t>(dataset.hrir_len) > k_max_fft - block_size + 1U ||
        dataset.dirs_deg.size() != static_cast<std::size_t>(dataset.num_dirs) * 2U) {
        return make_error(ErrorCode::invalid_argument, "invalid HRTF preparation dimensions");
    }
    auto bs = std::make_unique<BinauralState>();
    bs->num_dirs = dataset.num_dirs;
    bs->hrir_len = dataset.hrir_len;
    bs->fft_size = static_cast<int>(
        std::bit_ceil(static_cast<std::size_t>(block_size) + static_cast<std::size_t>(dataset.hrir_len) - 1U));
    bs->n_bands = (bs->fft_size / 2) + 1;
    bs->overlap_len = dataset.hrir_len - 1;
    bs->dataset_name = std::move(dataset.name);
    bs->hrtf_td = std::move(dataset.hrirs);
    bs->grid_dirs_deg = std::move(dataset.dirs_deg);
    bs->sample_rate = dataset.sample_rate;
    if (auto grid = HrtfGrid::create(bs->grid_dirs_deg)) {
        bs->grid = std::make_shared<HrtfGrid>(std::move(*grid));
    }
    if (!bs->grid) {
        return make_error(ErrorCode::render_failed, "HRTF direction triangulation failed");
    }
    auto filters = dsp::HrtfFilters::create(*bs->grid,
                                            bs->hrtf_td,
                                            static_cast<std::size_t>(bs->hrir_len),
                                            static_cast<std::size_t>(bs->fft_size),
                                            cache_magnitudes);
    if (!filters) {
        return tl::unexpected{filters.error()};
    }
    bs->filters = std::move(*filters);
    return bs;
}

// Private renderer adapters reuse caller buffers; interpolation and all
// frequency-domain state are owned by Rust. Diagnostics use that same kernel.
void compute_hrtf_into(const BinauralState& bs, float az_deg, float el_deg, std::vector<dsp::Complex>& out) {
    out.resize(bs.filters.output_size());
    const auto result = bs.filters.query(az_deg, el_deg, dsp::HrtfLookup::quantized, out);
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
}

void compute_continuous_hrtf_into(const BinauralState& bs, float az_deg, float el_deg, std::vector<dsp::Complex>& out) {
    out.resize(bs.filters.output_size());
    const auto result = bs.filters.query(az_deg, el_deg, dsp::HrtfLookup::continuous, out);
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
}

} // namespace mradm::binaural_internal
