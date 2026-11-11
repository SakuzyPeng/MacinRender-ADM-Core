#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "adm_fixture.h"
#include "wav_fixture.h"

namespace fixture {

// Round to the nearest nanosecond so importing the ADM times recovers the authored sample boundaries.
inline AudioBlockFormatDirectSpeakers
direct_speakers_block(std::string_view label, uint64_t start, uint64_t end, uint32_t sample_rate, float gain = 1.0F) {
    const auto time = [sample_rate](uint64_t frame) {
        return std::chrono::nanoseconds{static_cast<int64_t>((frame * 1000000000ULL + sample_rate / 2U) / sample_rate)};
    };
    AudioBlockFormatDirectSpeakers block{SphericalSpeakerPosition{Azimuth{0.0F}, Elevation{0.0F}, Distance{1.0F}}};
    block.add(SpeakerLabel{std::string{label}});
    block.set(Gain{gain});
    block.set(Rtime{Time{time(start)}});
    block.set(Duration{Time{time(end) - time(start)}});
    return block;
}

// A complete mono ADM input, authored independently of the production importer.
inline void write_mono_direct_speakers(const std::filesystem::path& path,
                                       uint32_t sample_rate,
                                       std::span<const float> samples,
                                       float object_gain,
                                       const std::vector<AudioBlockFormatDirectSpeakers>& blocks,
                                       std::optional<float> low_pass = std::nullopt) {
    auto channel =
        AudioChannelFormat::create(AudioChannelFormatName{"DirectSpeakersCF"}, TypeDefinition::direct_speakers);
    for (const auto& block : blocks) {
        channel->add(block);
    }
    if (low_pass) {
        Param frequency{"frequency", value_string(*low_pass)};
        frequency.attrs["typeDefinition"] = "lowPass";
        channel->set(frequency);
    }
    auto pack = AudioPackFormat::create(AudioPackFormatName{"DirectSpeakersPF"}, TypeDefinition::direct_speakers);
    pack->add_reference(channel);
    auto stream = AudioStreamFormat::create(AudioStreamFormatName{"DirectSpeakersSF"}, FormatDefinition::pcm);
    stream->set_reference(channel);
    auto track = AudioTrackFormat::create(AudioTrackFormatName{"DirectSpeakersTF"}, FormatDefinition::pcm);
    track->set_reference(stream);
    stream->add_reference(track);
    auto uid = AudioTrackUid::create();
    uid->set_reference(track);
    uid->set_reference(pack);
    auto object = AudioObject::create(AudioObjectName{"DirectSpeakersObject"});
    object->set(Gain{object_gain});
    object->add_reference(uid);
    auto content = AudioContent::create(AudioContentName{"DirectSpeakersContent"});
    content->add_reference(object);
    auto programme = AudioProgramme::create(AudioProgrammeName{"DirectSpeakersProgramme"});
    programme->add_reference(content);
    auto doc = Document::create();
    doc->add(programme);
    reassign_ids(doc);

    std::ostringstream xml;
    write_xml(xml, doc);
    auto chna =
        std::make_shared<WaveChna>(std::vector<WaveAudioId>{{1U, format_id(uid->get<AudioTrackUidId>()), "", ""}});
    auto axml = std::make_shared<WaveAxml>(xml.str());
    auto writer = write_wave(path.string(), 1U, sample_rate, 24U, chna, axml);
    writer->write(samples.data(), samples.size());
}

} // namespace fixture
