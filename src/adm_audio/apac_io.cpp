#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "adm/audio_io.h"

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "adm/errors.h"

namespace mradm::audio {

// Encode a fully post-processed float32 WAV to APAC in an MPEG-4 (.m4a / mp4f)
// container using the AudioToolbox ExtAudioFile C API.  On non-Apple platforms the
// function returns ErrorCode::unsupported immediately.
// NOLINTNEXTLINE(readability-function-size)
Result<void> convert_to_apac(const std::string& src_path,
                             const std::string& apac_path,
                             const std::string& layout_id,
                             uint32_t bitrate_kbps,
                             bool drc_music) {
#ifndef __APPLE__
    (void) src_path;
    (void) apac_path;
    (void) layout_id;
    (void) bitrate_kbps;
    (void) drc_music;
    return make_error(ErrorCode::unsupported, "APAC encoding requires macOS (AudioToolbox)", {});
#else
    struct ApacLayout {
        AudioChannelLayoutTag tag;
        bool wav71_swap;
        uint32_t expected_channels; // 0 = any channel count accepted
    };
    std::optional<ApacLayout> al;
    if (layout_id == "binaural") {
        // CoreAudio accepts the Binaural input layout here, but as of macOS 15
        // afinfo reports the encoded APAC track as Stereo (L R).  Keep the
        // layout id for validation/metadata semantics; do not rely on APAC as a
        // durable binaural channel-layout carrier.
        al = ApacLayout{kAudioChannelLayoutTag_Binaural, false, 2U};
    } else if (layout_id == "0+2+0") {
        al = ApacLayout{kAudioChannelLayoutTag_MPEG_2_0, false, 2U};
    } else if (layout_id == "wav71") {
        al = ApacLayout{kAudioChannelLayoutTag_AudioUnit_7_1, true, 8U};
    } else if (layout_id == "hoa3") {
        al = ApacLayout{static_cast<AudioChannelLayoutTag>((190U << 16U) | 16U), false, 16U};
    } else if (layout_id == "4+5+0" || layout_id == "5.1.4" || layout_id == "atmos514") {
        al = ApacLayout{static_cast<AudioChannelLayoutTag>((195U << 16U) | 10U), false, 10U};
    } else if (layout_id == "4+7+0" || layout_id == "7.1.4" || layout_id == "atmos714") {
        al = ApacLayout{static_cast<AudioChannelLayoutTag>((192U << 16U) | 12U), false, 12U};
    } else if (layout_id == "9.1.6" || layout_id == "atmos916") {
        al = ApacLayout{static_cast<AudioChannelLayoutTag>((193U << 16U) | 16U), false, 16U};
    } else if (layout_id == "9+10+3" || layout_id == "22.2" || layout_id == "cicp13") {
        al = ApacLayout{static_cast<AudioChannelLayoutTag>((204U << 16U) | 24U), false, 24U};
    } else {
        return make_error(ErrorCode::unsupported, fmt::format("APAC: unsupported layout '{}'", layout_id), {});
    }

    auto reader_res = FloatWavReader::open(src_path);
    if (!reader_res) {
        return tl::unexpected{reader_res.error()};
    }
    auto& reader = *reader_res;
    const uint32_t channels = reader.channels();

    if (al->expected_channels > 0U && channels != al->expected_channels) {
        return make_error(
            ErrorCode::invalid_argument,
            fmt::format("APAC layout '{}' requires {} channels, got {}", layout_id, al->expected_channels, channels),
            "src=" + src_path);
    }

    if (reader.sample_rate() != 48000U) {
        return make_error(ErrorCode::invalid_argument,
                          fmt::format("APAC requires 48000 Hz input, got {} Hz", reader.sample_rate()),
                          "src=" + src_path);
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr,
                                                           reinterpret_cast<const UInt8*>(apac_path.c_str()),
                                                           static_cast<CFIndex>(apac_path.size()),
                                                           static_cast<Boolean>(false));
    if (url == nullptr) {
        return make_error(ErrorCode::io_error, "APAC: CFURLCreate failed", "path=" + apac_path);
    }

    // FourCC 'apac' = 0x61706163; use hex to avoid -Wmultichar in C++.
    constexpr AudioFormatID k_apac_fmt = 0x61706163U;
    AudioStreamBasicDescription apac_asbd{};
    apac_asbd.mSampleRate = 48000.0;
    apac_asbd.mFormatID = k_apac_fmt;
    apac_asbd.mChannelsPerFrame = channels;

    AudioChannelLayout file_layout{};
    file_layout.mChannelLayoutTag = al->tag;

    ExtAudioFileRef ext_file = nullptr;
    OSStatus err = ExtAudioFileCreateWithURL(
        url, kAudioFileMPEG4Type, &apac_asbd, &file_layout, kAudioFileFlags_EraseFile, &ext_file);
    CFRelease(url);
    if (err != noErr) {
        return make_error(ErrorCode::io_error,
                          fmt::format("APAC: ExtAudioFileCreateWithURL failed ({})", static_cast<int>(err)),
                          "path=" + apac_path);
    }

    AudioStreamBasicDescription src_asbd{};
    src_asbd.mSampleRate = 48000.0;
    src_asbd.mFormatID = kAudioFormatLinearPCM;
    src_asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    src_asbd.mChannelsPerFrame = channels;
    src_asbd.mBitsPerChannel = 32;
    src_asbd.mFramesPerPacket = 1;
    src_asbd.mBytesPerFrame = static_cast<UInt32>(sizeof(float) * channels);
    src_asbd.mBytesPerPacket = src_asbd.mBytesPerFrame;
    err = ExtAudioFileSetProperty(ext_file, kExtAudioFileProperty_ClientDataFormat, sizeof(src_asbd), &src_asbd);
    if (err != noErr) {
        ExtAudioFileDispose(ext_file);
        return make_error(ErrorCode::io_error,
                          fmt::format("APAC: ClientDataFormat failed ({})", static_cast<int>(err)),
                          "path=" + apac_path);
    }

    // APAC-specific property IDs (not yet named in SDK headers; hex avoids -Wmultichar).
    // 'csrc'=0x63737263 (source type 7=Spatial Offline, fixed)
    // 'cdrc'=0x63647263 (DRC profile: 0=None, 1=Music)
    // 'aspf'=0x61737066 (sync packet frequency 75, fixed)
    constexpr AudioConverterPropertyID k_apac_csrc = 0x63737263U;
    constexpr AudioConverterPropertyID k_apac_cdrc = 0x63647263U;
    constexpr AudioConverterPropertyID k_apac_aspf = 0x61737066U;

    AudioConverterRef conv = nullptr;
    UInt32 conv_sz = sizeof(AudioConverterRef);
    // NOLINTNEXTLINE(bugprone-multi-level-implicit-pointer-conversion)
    err = ExtAudioFileGetProperty(ext_file, kExtAudioFileProperty_AudioConverter, &conv_sz, &conv);
    if (err != noErr || conv == nullptr) {
        ExtAudioFileDispose(ext_file);
        return make_error(ErrorCode::io_error,
                          fmt::format("APAC: AudioConverter lookup failed ({})", static_cast<int>(err)),
                          "path=" + apac_path);
    }
    if (bitrate_kbps > 0U) {
        UInt32 br = bitrate_kbps * 1000U;
        err = AudioConverterSetProperty(conv, kAudioConverterEncodeBitRate, sizeof(br), &br);
        if (err != noErr) {
            ExtAudioFileDispose(ext_file);
            return make_error(ErrorCode::io_error,
                              fmt::format("APAC: EncodeBitRate failed ({})", static_cast<int>(err)),
                              "path=" + apac_path);
        }
    }
    constexpr uint32_t k_csrc_val = 7U;
    constexpr uint32_t k_aspf_val = 75U;
    const uint32_t cdrc_val = drc_music ? 1U : 0U;
    err = AudioConverterSetProperty(conv, k_apac_csrc, sizeof(k_csrc_val), &k_csrc_val);
    if (err != noErr) {
        ExtAudioFileDispose(ext_file);
        return make_error(ErrorCode::io_error,
                          fmt::format("APAC: csrc property failed ({})", static_cast<int>(err)),
                          "path=" + apac_path);
    }
    err = AudioConverterSetProperty(conv, k_apac_cdrc, sizeof(cdrc_val), &cdrc_val);
    if (err != noErr) {
        ExtAudioFileDispose(ext_file);
        return make_error(ErrorCode::io_error,
                          fmt::format("APAC: cdrc property failed ({})", static_cast<int>(err)),
                          "path=" + apac_path);
    }
    err = AudioConverterSetProperty(conv, k_apac_aspf, sizeof(k_aspf_val), &k_aspf_val);
    if (err != noErr) {
        ExtAudioFileDispose(ext_file);
        return make_error(ErrorCode::io_error,
                          fmt::format("APAC: aspf property failed ({})", static_cast<int>(err)),
                          "path=" + apac_path);
    }
    CFArrayRef empty = CFArrayCreate(nullptr, nullptr, 0, nullptr);
    if (empty == nullptr) {
        ExtAudioFileDispose(ext_file);
        return make_error(ErrorCode::io_error, "APAC: CFArrayCreate failed (OOM?)", "path=" + apac_path);
    }
    // NOLINTNEXTLINE(bugprone-sizeof-expression,bugprone-multi-level-implicit-pointer-conversion)
    err = ExtAudioFileSetProperty(ext_file, kExtAudioFileProperty_ConverterConfig, sizeof(CFArrayRef), &empty);
    CFRelease(empty);
    if (err != noErr) {
        ExtAudioFileDispose(ext_file);
        return make_error(ErrorCode::io_error,
                          fmt::format("APAC: ConverterConfig commit failed ({})", static_cast<int>(err)),
                          "path=" + apac_path);
    }

    constexpr uint64_t k_block = 4096;
    std::vector<float> buf(static_cast<std::size_t>(channels) * k_block);
    uint64_t left = reader.frame_count();
    while (left > 0) {
        const uint64_t n = std::min(k_block, left);
        const uint64_t got = reader.read(buf.data(), n);
        if (got == 0) {
            break;
        }
        if (al->wav71_swap) {
            for (uint64_t f = 0; f < got; ++f) {
                float* fr = buf.data() + (f * channels);
                std::swap(fr[4], fr[6]);
                std::swap(fr[5], fr[7]);
            }
        }
        AudioBufferList abl{};
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = channels;
        abl.mBuffers[0].mDataByteSize = static_cast<UInt32>(got * static_cast<uint64_t>(channels) * sizeof(float));
        abl.mBuffers[0].mData = buf.data();
        err = ExtAudioFileWrite(ext_file, static_cast<UInt32>(got), &abl);
        if (err != noErr) {
            ExtAudioFileDispose(ext_file);
            return make_error(ErrorCode::io_error,
                              fmt::format("APAC: ExtAudioFileWrite failed ({})", static_cast<int>(err)),
                              "path=" + apac_path);
        }
        left -= got;
    }
    if (left != 0) {
        ExtAudioFileDispose(ext_file);
        return make_error(ErrorCode::io_error, "short read in convert_to_apac", "src=" + src_path);
    }

    err = ExtAudioFileDispose(ext_file);
    if (err != noErr) {
        return make_error(ErrorCode::io_error,
                          fmt::format("APAC: ExtAudioFileDispose failed ({})", static_cast<int>(err)),
                          "path=" + apac_path);
    }
    return {};
#endif
}


} // namespace mradm::audio
