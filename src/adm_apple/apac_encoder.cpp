#include "apac_encoder.h"

#include <cstdio>
#include <memory>

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

namespace {

void* open_encoder(MradmAdmText path,
                   uint32_t layout_tag,
                   uint32_t bitrate_kbps,
                   uint32_t drc_music,
                   uint32_t caf,
                   char* message,
                   size_t capacity) noexcept {
    const auto fail = [&](const char* operation, OSStatus status) -> void* {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        std::snprintf(message, capacity, "APAC: %s failed (%d)", operation, static_cast<int>(status));
        return nullptr;
    };
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        nullptr, path.data, static_cast<CFIndex>(path.len), static_cast<Boolean>(false));
    if (url == nullptr) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        std::snprintf(message, capacity, "APAC: CFURLCreate failed");
        return nullptr;
    }

    AudioStreamBasicDescription apac{};
    apac.mSampleRate = 48000.0;
    apac.mFormatID = 0x61706163U; // 'apac'
    apac.mChannelsPerFrame = AudioChannelLayoutTag_GetNumberOfChannels(layout_tag);
    AudioChannelLayout layout{};
    layout.mChannelLayoutTag = layout_tag;
    ExtAudioFileRef raw = nullptr;
    OSStatus status = ExtAudioFileCreateWithURL(
        url, caf != 0U ? kAudioFileCAFType : kAudioFileMPEG4Type, &apac, &layout, kAudioFileFlags_EraseFile, &raw);
    CFRelease(url);
    if (status != noErr) {
        return fail("ExtAudioFileCreateWithURL", status);
    }
    std::unique_ptr<OpaqueExtAudioFile, decltype(&ExtAudioFileDispose)> file(raw, &ExtAudioFileDispose);

    AudioStreamBasicDescription pcm{};
    pcm.mSampleRate = apac.mSampleRate;
    pcm.mFormatID = kAudioFormatLinearPCM;
    pcm.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    pcm.mChannelsPerFrame = apac.mChannelsPerFrame;
    pcm.mBitsPerChannel = 32;
    pcm.mFramesPerPacket = 1;
    pcm.mBytesPerFrame = static_cast<UInt32>(sizeof(float)) * pcm.mChannelsPerFrame;
    pcm.mBytesPerPacket = pcm.mBytesPerFrame;
    status = ExtAudioFileSetProperty(raw, kExtAudioFileProperty_ClientDataFormat, sizeof(pcm), &pcm);
    if (status != noErr) {
        return fail("ClientDataFormat", status);
    }
    AudioConverterRef converter = nullptr;
    UInt32 size = sizeof(AudioConverterRef);
    // NOLINTNEXTLINE(bugprone-multi-level-implicit-pointer-conversion)
    status = ExtAudioFileGetProperty(raw, kExtAudioFileProperty_AudioConverter, &size, &converter);
    if (status != noErr || converter == nullptr) {
        return fail("AudioConverter lookup", status);
    }
    if (bitrate_kbps != 0U) {
        const UInt32 bitrate = bitrate_kbps * 1000U;
        status = AudioConverterSetProperty(converter, kAudioConverterEncodeBitRate, sizeof(bitrate), &bitrate);
        if (status != noErr) {
            return fail("EncodeBitRate", status);
        }
    }
    constexpr UInt32 k_spatial_offline = 7;
    constexpr UInt32 k_sync_frequency = 75;
    status = AudioConverterSetProperty(converter, 0x63737263U, sizeof(k_spatial_offline), &k_spatial_offline);
    if (status != noErr) {
        return fail("csrc property", status);
    }
    status = AudioConverterSetProperty(converter, 0x63647263U, sizeof(drc_music), &drc_music);
    if (status != noErr) {
        return fail("cdrc property", status);
    }
    status = AudioConverterSetProperty(converter, 0x61737066U, sizeof(k_sync_frequency), &k_sync_frequency);
    if (status != noErr) {
        return fail("aspf property", status);
    }
    CFArrayRef empty = CFArrayCreate(nullptr, nullptr, 0, nullptr);
    if (empty == nullptr) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        std::snprintf(message, capacity, "APAC: CFArrayCreate failed (OOM?)");
        return nullptr;
    }
    // NOLINTNEXTLINE(bugprone-sizeof-expression,bugprone-multi-level-implicit-pointer-conversion)
    status = ExtAudioFileSetProperty(raw, kExtAudioFileProperty_ConverterConfig, sizeof(empty), &empty);
    CFRelease(empty);
    return status == noErr ? file.release() : fail("ConverterConfig commit", status);
}

int32_t write_frames(void* file, float* interleaved, uint32_t channels, uint32_t frames) noexcept {
    AudioBufferList buffers{};
    buffers.mNumberBuffers = 1;
    buffers.mBuffers[0] = {channels, frames * channels * static_cast<uint32_t>(sizeof(float)), interleaved};
    return ExtAudioFileWrite(static_cast<ExtAudioFileRef>(file), frames, &buffers);
}

int32_t finish(void* file) noexcept {
    return ExtAudioFileDispose(static_cast<ExtAudioFileRef>(file));
}

constexpr MradmApacEncoder k_encoder{&open_encoder, &write_frames, &finish};

} // namespace

extern "C" const MradmApacEncoder* mradm_apple_apac_encoder(void) {
    return &k_encoder;
}
