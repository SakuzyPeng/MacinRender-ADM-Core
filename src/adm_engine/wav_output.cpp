#include "wav_output.h"

#include <array>
#include <cstdint>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "adm/errors.h"

#include "audio_io_internal.h"
#include "metadata.h"
#include "speaker_layouts.h"

namespace mradm::engine {

namespace {

metadata::OutputMetadata
make_adm(uint32_t kind, std::string_view name, const std::vector<render_layouts::SpeakerSpec>& speakers = {}) {
    auto result = metadata::generate(kind, name, speakers);
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    return std::move(*result);
}
metadata::OutputMetadata make_binaural_adm() {
    return make_adm(5U, "Binaural");
}
metadata::OutputMetadata make_hoa3_adm() {
    return make_adm(4U, "HOA3 ACN SN3D");
}
metadata::OutputMetadata make_direct_speakers_adm(std::string_view layout_id) {
    const auto* layout = render_layouts::find_speaker_layout(layout_id);
    if (layout == nullptr) {
        throw std::runtime_error(fmt::format("speaker layout '{}' is unavailable", layout_id));
    }
    return make_adm(1U, layout->display_name, layout->speakers);
}

audio::WavLayoutFinalization make_wav_layout_finalization(std::string_view raw_layout, WavLayoutProfile profile) {
    audio::WavLayoutFinalization output;
    if (profile == WavLayoutProfile::speaker_rerender) {
        if (raw_layout != "4+7+0" && raw_layout != "7.1.4" && raw_layout != "9.1.6") {
            throw std::runtime_error("speaker re-render WAV profile requires 7.1.4 or 9.1.6");
        }
        output.force_extensible = true;
        output.prefer_riff = true;
        output.include_pcm_fact = true;
        // The 9.1.6 compatibility profile omits the speaker mask and rendered ADM.
        // CHNA would make CoreAudio expose an ADM/discrete 16.0 track instead.
        // The samples remain in the verified 9.1.6 native order. CAF carries
        // the explicit CoreAudio Atmos_9_1_6 tag for layout-aware playback.
        if (raw_layout == "9.1.6") {
            return output;
        }
    }
    if (raw_layout == "0+2+0") {
        output.channel_mask = 0x0003U;
    } else if (raw_layout == "0+5+0" || raw_layout == "5.1") {
        output.channel_mask = 0x003FU;
    } else if (raw_layout == "2+5+0" || raw_layout == "5.1.2") {
        output.channel_mask = 0x503FU;
    } else if (raw_layout == "wav71" || raw_layout == "7.1") {
        output.channel_mask = 0x063FU;
    } else if (raw_layout == "4+5+0" || raw_layout == "5.1.4") {
        output.channel_mask = 0x2D03FU;
    } else if (raw_layout == "4+7+0" || raw_layout == "7.1.4") {
        output.channel_mask = 0x2D63FU;
        // Renderer/Atmos order has side L/R before rear L/R. WAVE mask order
        // is ascending bit position: rear L/R before side L/R.
        output.output_channel_sources = {0U, 1U, 2U, 3U, 6U, 7U, 4U, 5U, 8U, 9U, 10U, 11U};
    } else if (raw_layout == "binaural") {
        auto metadata = make_binaural_adm();
        output.axml = std::move(metadata.axml);
        output.chna = std::move(metadata.chna);
    } else if (raw_layout == "4+5+4" || raw_layout == "9.1.4") {
        auto metadata = make_direct_speakers_adm("4+5+4");
        output.axml = std::move(metadata.axml);
        output.chna = std::move(metadata.chna);
    } else if (raw_layout == "9.1.6") {
        auto metadata = make_direct_speakers_adm("9.1.6");
        output.axml = std::move(metadata.axml);
        output.chna = std::move(metadata.chna);
    } else if (raw_layout == "9+10+3" || raw_layout == "22.2") {
        auto metadata = make_direct_speakers_adm("9+10+3");
        output.axml = std::move(metadata.axml);
        output.chna = std::move(metadata.chna);
    } else if (raw_layout == "hoa3") {
        auto metadata = make_hoa3_adm();
        output.axml = std::move(metadata.axml);
        output.chna = std::move(metadata.chna);
    } else {
        throw std::runtime_error(fmt::format("WAV output layout '{}' has no serialization definition", raw_layout));
    }
    return output;
}

} // namespace

Result<void> finalize_rendered_wav(const std::string& path,
                                   const std::string& output_layout,
                                   const std::stop_token& cancel_token,
                                   ProgressSink* progress,
                                   WavLayoutProfile profile) {
    try {
        auto layout = make_wav_layout_finalization(output_layout, profile);
        return audio::finalize_wav_layout(path, layout, cancel_token, progress, RenderOperation::write_metadata);
    } catch (const std::exception& error) {
        return make_error(ErrorCode::io_error,
                          std::string{"failed to build WAV layout metadata: "} + error.what(),
                          "path=" + path + " layout=" + output_layout);
    } catch (...) {
        return make_error(ErrorCode::internal_error,
                          "failed to build WAV layout metadata",
                          "path=" + path + " layout=" + output_layout);
    }
}

} // namespace mradm::engine
