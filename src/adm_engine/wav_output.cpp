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

#include <adm/adm.hpp>
#include <adm/utilities/id_assignment.hpp>
#include <adm/write.hpp>
#include <fmt/format.h>

#include "adm/errors.h"

#include "audio_io_internal.h"
#include "speaker_layouts.h"

namespace mradm::engine {

namespace {

struct AdmTrackChain {
    std::shared_ptr<adm::AudioTrackUid> uid;
    std::shared_ptr<adm::AudioTrackFormat> track_format;
};

struct AdmOutputMetadata {
    std::string axml;
    std::vector<audio::WavChnaEntry> chna;
};

AdmTrackChain add_track_chain(const std::shared_ptr<adm::Document>& document,
                              const std::shared_ptr<adm::AudioPackFormat>& pack_format,
                              const std::shared_ptr<adm::AudioChannelFormat>& channel_format,
                              const std::shared_ptr<adm::AudioObject>& object,
                              std::string_view name) {
    auto stream_format = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{std::string{name} + " Stream"},
                                                        adm::FormatDefinition::PCM);
    stream_format->setReference(channel_format);
    document->add(stream_format);

    auto track_format = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{std::string{name} + " Track"},
                                                      adm::FormatDefinition::PCM);
    track_format->setReference(stream_format);
    stream_format->addReference(track_format);
    document->add(track_format);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(track_format);
    uid->setReference(pack_format);
    document->add(uid);
    object->addReference(uid);
    return {std::move(uid), std::move(track_format)};
}

AdmOutputMetadata finish_adm_document(const std::shared_ptr<adm::Document>& document,
                                      const std::shared_ptr<adm::AudioPackFormat>& pack_format,
                                      const std::vector<AdmTrackChain>& tracks) {
    adm::reassignIds(document);

    AdmOutputMetadata metadata;
    metadata.chna.reserve(tracks.size());
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        metadata.chna.push_back({static_cast<uint16_t>(index + 1U),
                                 adm::formatId(tracks[index].uid->get<adm::AudioTrackUidId>()),
                                 adm::formatId(tracks[index].track_format->get<adm::AudioTrackFormatId>()),
                                 adm::formatId(pack_format->get<adm::AudioPackFormatId>())});
    }

    std::ostringstream xml;
    adm::writeXml(xml, document);
    metadata.axml = xml.str();
    return metadata;
}

void add_programme_hierarchy(const std::shared_ptr<adm::Document>& document,
                             const std::shared_ptr<adm::AudioObject>& object,
                             std::string_view name) {
    auto content = adm::AudioContent::create(adm::AudioContentName{std::string{name} + " Content"});
    content->addReference(object);
    document->add(content);

    auto programme = adm::AudioProgramme::create(adm::AudioProgrammeName{std::string{name} + " Programme"});
    programme->addReference(content);
    document->add(programme);
}

AdmOutputMetadata make_binaural_adm() {
    auto document = adm::Document::create();
    auto pack_format =
        adm::AudioPackFormat::create(adm::AudioPackFormatName{"Binaural"}, adm::TypeDefinition::BINAURAL);
    document->add(pack_format);
    auto object = adm::AudioObject::create(adm::AudioObjectName{"Binaural Render"});
    document->add(object);

    constexpr std::array<std::string_view, 2> k_ear_names{"leftEar", "rightEar"};
    std::vector<AdmTrackChain> tracks;
    tracks.reserve(k_ear_names.size());
    for (const auto name : k_ear_names) {
        auto channel_format = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{std::string{name}},
                                                              adm::TypeDefinition::BINAURAL);
        channel_format->add(adm::AudioBlockFormatBinaural{});
        document->add(channel_format);
        pack_format->addReference(channel_format);
        tracks.push_back(add_track_chain(document, pack_format, channel_format, object, name));
    }
    add_programme_hierarchy(document, object, "Binaural Render");
    return finish_adm_document(document, pack_format, tracks);
}

AdmOutputMetadata make_direct_speakers_adm(std::string_view layout_id) {
    const auto* layout = render_layouts::find_speaker_layout(layout_id);
    if (layout == nullptr) {
        throw std::runtime_error(fmt::format("speaker layout '{}' is unavailable", layout_id));
    }

    auto document = adm::Document::create();
    auto pack_format = adm::AudioPackFormat::create(adm::AudioPackFormatName{std::string{layout->display_name}},
                                                    adm::TypeDefinition::DIRECT_SPEAKERS);
    document->add(pack_format);
    auto object = adm::AudioObject::create(adm::AudioObjectName{std::string{layout->display_name} + " Render"});
    document->add(object);

    std::vector<AdmTrackChain> tracks;
    tracks.reserve(layout->speakers.size());
    for (std::size_t index = 0; index < layout->speakers.size(); ++index) {
        const auto& speaker = layout->speakers[index];
        const auto channel_name = fmt::format("{} {}", speaker.label, index + 1U);
        auto channel_format = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{channel_name},
                                                              adm::TypeDefinition::DIRECT_SPEAKERS);

        adm::SphericalSpeakerPosition position{
            adm::Azimuth{speaker.azimuth}, adm::Elevation{speaker.elevation}, adm::Distance{1.0F}};
        if (speaker.azimuth_range.has_value()) {
            position.set(adm::AzimuthMin{speaker.azimuth_range->first});
            position.set(adm::AzimuthMax{speaker.azimuth_range->second});
        }
        if (speaker.elevation_range.has_value()) {
            position.set(adm::ElevationMin{speaker.elevation_range->first});
            position.set(adm::ElevationMax{speaker.elevation_range->second});
        }
        adm::AudioBlockFormatDirectSpeakers block{position};
        block.add(adm::SpeakerLabel{std::string{speaker.label}});
        channel_format->add(block);
        if (speaker.is_lfe) {
            channel_format->set(adm::Frequency{adm::LowPass{120.0F}});
        }
        document->add(channel_format);
        pack_format->addReference(channel_format);
        tracks.push_back(add_track_chain(document, pack_format, channel_format, object, channel_name));
    }
    add_programme_hierarchy(document, object, std::string{layout->display_name} + " Render");
    return finish_adm_document(document, pack_format, tracks);
}

AdmOutputMetadata make_hoa3_adm() {
    constexpr uint32_t k_channels = 16U;
    auto document = adm::Document::create();
    auto hoa_pack =
        adm::AudioPackFormatHoa::create(adm::AudioPackFormatName{"HOA3 ACN SN3D"}, adm::Normalization{"SN3D"});
    document->add(hoa_pack);
    auto object = adm::AudioObject::create(adm::AudioObjectName{"HOA3 Render"});
    document->add(object);

    std::vector<AdmTrackChain> tracks;
    tracks.reserve(k_channels);
    for (uint32_t acn = 0; acn < k_channels; ++acn) {
        const auto signed_acn = static_cast<int>(acn);
        int order = 0;
        while ((order + 1) * (order + 1) <= signed_acn) {
            ++order;
        }
        const int degree = signed_acn - (order * (order + 1));
        const auto name = fmt::format("ACN {}", acn);
        auto channel_format =
            adm::AudioChannelFormat::create(adm::AudioChannelFormatName{name}, adm::TypeDefinition::HOA);
        channel_format->add(
            adm::AudioBlockFormatHoa{adm::Order{order}, adm::Degree{degree}, adm::Normalization{"SN3D"}});
        document->add(channel_format);
        hoa_pack->addReference(channel_format);
        tracks.push_back(add_track_chain(document, hoa_pack, channel_format, object, name));
    }
    add_programme_hierarchy(document, object, "HOA3 Render");
    return finish_adm_document(document, hoa_pack, tracks);
}

audio::WavLayoutFinalization make_wav_layout_finalization(std::string_view raw_layout) {
    audio::WavLayoutFinalization output;
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
                                   ProgressSink* progress) {
    try {
        auto layout = make_wav_layout_finalization(output_layout);
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
