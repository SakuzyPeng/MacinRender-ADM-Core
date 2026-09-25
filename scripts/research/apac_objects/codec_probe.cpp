// Standalone macOS research tool. Private properties are version-specific.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

extern "C" {
// Only trace_codec.py writes/substitutes this table when explicitly requested.
__attribute__((used)) unsigned int g_apac_diagnostic_profiles[2] = {5U, (6U << 16U) | 31U};
__attribute__((used)) unsigned long long g_probe_decode_packet_index = 0;
__attribute__((used)) unsigned long long g_probe_pcm_output_frames = 0;
}

namespace {
constexpr UInt32 apac_format = 0x61706163U;
constexpr UInt32 custom_mode = 0x63756d6fU;
constexpr UInt32 asc_settings = 0x61637320U;

bool status(const char* stage, OSStatus value) {
    std::printf("{\"stage\":\"%s\",\"status\":%d}\n", stage, static_cast<int>(value));
    std::fflush(stdout);
    return value == noErr;
}

bool write_plist(CFPropertyListRef value, const char* path) {
    if (std::filesystem::exists(path))
        return false;
    CFErrorRef error = nullptr;
    auto data = CFPropertyListCreateData(nullptr, value, kCFPropertyListXMLFormat_v1_0, 0, &error);
    if (!data) {
        if (error)
            CFRelease(error);
        return false;
    }
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(CFDataGetBytePtr(data)), CFDataGetLength(data));
    CFRelease(data);
    return file.good();
}

CFPropertyListRef read_plist(const char* path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.empty())
        return nullptr;
    auto data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(bytes.data()), bytes.size());
    CFErrorRef error = nullptr;
    auto value = CFPropertyListCreateWithData(nullptr, data, kCFPropertyListImmutable, nullptr, &error);
    CFRelease(data);
    if (error)
        CFRelease(error);
    return value;
}

void inspect_decoder_layout(AudioCodec codec, AudioCodecPropertyID property, const char* stage) {
    UInt32 size = 0;
    if (!status(stage, AudioCodecGetPropertyInfo(codec, property, &size, nullptr)) || size < 12 || size > 65536)
        return;
    std::vector<unsigned char> storage(size);
    if (!status(stage, AudioCodecGetProperty(codec, property, &size, storage.data())))
        return;
    const auto* layout = reinterpret_cast<const AudioChannelLayout*>(storage.data());
    std::printf("{\"stage\":\"%s_value\",\"tag\":%u,\"descriptions\":[", stage, layout->mChannelLayoutTag);
    for (UInt32 i = 0; i < layout->mNumberChannelDescriptions && 12U + 20U * (i + 1U) <= size; ++i) {
        const auto& channel = layout->mChannelDescriptions[i];
        std::printf("%s{\"label\":%u,\"flags\":%u}", i ? "," : "", channel.mChannelLabel, channel.mChannelFlags);
    }
    std::puts("]}");
}

int decode(const std::string& prefix, const char* output_path, UInt32 channels) {
    if (!channels || channels > 256 || std::filesystem::exists(output_path))
        return 2;
    AudioStreamBasicDescription input{};
    std::ifstream format_file(prefix + ".asbd", std::ios::binary);
    if (!format_file.read(reinterpret_cast<char*>(&input), sizeof(input)))
        return 2;
    std::ifstream cookie_file(prefix + ".cookie", std::ios::binary);
    std::vector<char> cookie((std::istreambuf_iterator<char>(cookie_file)), std::istreambuf_iterator<char>());
    std::ifstream encoded_file(prefix + ".packets", std::ios::binary);
    std::ifstream index_file(prefix + ".packet_index.jsonl");
    if (!encoded_file || !index_file || cookie.empty())
        return 2;
    AudioComponentDescription description{};
    description.componentType = kAudioDecoderComponentType;
    description.componentSubType = apac_format;
    description.componentManufacturer = kAudioUnitManufacturer_Apple;
    auto component = AudioComponentFindNext(nullptr, &description);
    AudioCodec codec = nullptr;
    if (!component || !status("decoder_create", AudioComponentInstanceNew(component, &codec)))
        return 1;
    AudioStreamBasicDescription pcm{};
    pcm.mSampleRate = input.mSampleRate;
    pcm.mFormatID = kAudioFormatLinearPCM;
    pcm.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    pcm.mBytesPerPacket = pcm.mBytesPerFrame = channels * sizeof(float);
    pcm.mFramesPerPacket = 1;
    pcm.mChannelsPerFrame = channels;
    pcm.mBitsPerChannel = 32;
    // Decoder capabilities and private configuration remain at system defaults.
    if (!status("decoder_initialize", AudioCodecInitialize(codec, &input, &pcm, cookie.data(), cookie.size())))
        return 1;
    inspect_decoder_layout(codec, kAudioCodecPropertyCurrentInputChannelLayout, "decoder_input_layout");
    inspect_decoder_layout(codec, kAudioCodecPropertyCurrentOutputChannelLayout, "decoder_output_layout");
    AudioStreamBasicDescription actual_format{};
    UInt32 actual_size = sizeof(actual_format);
    if (status("decoder_output_format",
               AudioCodecGetProperty(codec, kAudioCodecPropertyCurrentOutputFormat, &actual_size, &actual_format))) {
        std::printf("{\"stage\":\"decoder_output_format_value\",\"channels\":%u,\"bytes_per_frame\":%u,"
                    "\"frames_per_packet\":%u,\"flags\":%u}\n",
                    actual_format.mChannelsPerFrame,
                    actual_format.mBytesPerFrame,
                    actual_format.mFramesPerPacket,
                    actual_format.mFormatFlags);
    }
    AudioCodecPrimeInfo prime{};
    UInt32 property_size = sizeof(prime);
    if (status("decoder_prime", AudioCodecGetProperty(codec, kAudioCodecPropertyPrimeInfo, &property_size, &prime))) {
        std::printf("{\"stage\":\"decoder_prime_value\",\"leading\":%u,\"trailing\":%u}\n",
                    prime.leadingFrames,
                    prime.trailingFrames);
    }
    std::ofstream decoded(output_path, std::ios::binary);
    if (!decoded)
        return 2;
    std::vector<float> output(static_cast<std::size_t>(channels) * 4096);
    double energy = 0;
    std::string line;
    while (std::getline(index_file, line)) {
        unsigned long long offset = 0;
        UInt32 bytes = 0, frames = 0;
        if (std::sscanf(line.c_str(), "{\"offset\":%llu,\"bytes\":%u,\"frames\":%u}", &offset, &bytes, &frames) != 3)
            return 2;
        std::vector<char> packet(bytes);
        encoded_file.seekg(offset);
        if (!encoded_file.read(packet.data(), bytes))
            return 2;
        AudioStreamPacketDescription packet_description{0, frames ? frames : input.mFramesPerPacket, bytes};
        UInt32 consumed = bytes, packets = 1;
        if (!status("decoder_append",
                    AudioCodecAppendInputData(codec, packet.data(), &consumed, &packets, &packet_description)))
            return 1;
        std::printf(
            "{\"stage\":\"decoder_consumed\",\"offered\":%u,\"bytes\":%u,\"packets\":%u}\n", bytes, consumed, packets);
        if (consumed != bytes || packets != 1)
            return 1;
        for (unsigned iteration = 0; iteration < 16; ++iteration) {
            // This APAC implementation interprets the request as codec packets,
            // then returns PCM frames. Request one packet with enough PCM space.
            UInt32 capacity = output.size() * sizeof(float), output_frames = 1, result = 0;
            if (!status(
                    "decoder_produce",
                    AudioCodecProduceOutputPackets(codec, output.data(), &capacity, &output_frames, nullptr, &result)))
                return 1;
            if (capacity != output_frames * pcm.mBytesPerFrame)
                return 1;
            decoded.write(reinterpret_cast<const char*>(output.data()), capacity);
            for (UInt32 i = 0; i < capacity / sizeof(float); ++i)
                energy += double(output[i]) * output[i];
            g_probe_pcm_output_frames += output_frames;
            std::printf("{\"stage\":\"decoded_block\",\"packet_index\":%llu,\"frames\":%u,\"total_frames\":%llu,"
                        "\"result\":%u}\n",
                        g_probe_decode_packet_index,
                        output_frames,
                        g_probe_pcm_output_frames,
                        result);
            if (result != kAudioCodecProduceOutputPacketSuccessHasMore)
                break;
        }
        ++g_probe_decode_packet_index;
    }
    std::printf("{\"stage\":\"decoded\",\"frames\":%llu,\"channels\":%u,\"rms\":%.12g}\n",
                g_probe_pcm_output_frames,
                channels,
                g_probe_pcm_output_frames ? std::sqrt(energy / (g_probe_pcm_output_frames * channels)) : 0);
    AudioCodecUninitialize(codec);
    AudioComponentInstanceDispose(codec);
    return g_probe_pcm_output_frames && decoded.good() ? 0 : 1;
}

int import_file(const char* path, const std::string& prefix) {
    for (const char* suffix : {".asbd", ".cookie", ".timing.json", ".packets", ".packet_index.jsonl"}) {
        if (std::filesystem::exists(prefix + suffix))
            return 2;
    }
    auto url = CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8*>(path), std::strlen(path), false);
    AudioFileID file = nullptr;
    auto opened = status("file_open", AudioFileOpenURL(url, kAudioFileReadPermission, 0, &file));
    CFRelease(url);
    if (!opened)
        return 1;
    AudioStreamBasicDescription format{};
    UInt32 size = sizeof(format);
    if (!status("file_format", AudioFileGetProperty(file, kAudioFilePropertyDataFormat, &size, &format)))
        return 1;
    if (!status("file_cookie_info", AudioFileGetPropertyInfo(file, kAudioFilePropertyMagicCookieData, &size, nullptr)))
        return 1;
    std::vector<unsigned char> cookie(size);
    if (!status("file_cookie", AudioFileGetProperty(file, kAudioFilePropertyMagicCookieData, &size, cookie.data())))
        return 1;
    std::ofstream cookie_file(prefix + ".cookie", std::ios::binary);
    cookie_file.write(reinterpret_cast<const char*>(cookie.data()), size);
    std::ofstream format_file(prefix + ".asbd", std::ios::binary);
    format_file.write(reinterpret_cast<const char*>(&format), sizeof(format));
    AudioFilePacketTableInfo timing{};
    size = sizeof(timing);
    if (!status("file_timing", AudioFileGetProperty(file, kAudioFilePropertyPacketTableInfo, &size, &timing)))
        return 1;
    std::ofstream timing_file(prefix + ".timing.json");
    timing_file << "{\"valid_frames\":" << timing.mNumberValidFrames << ",\"leading_frames\":" << timing.mPrimingFrames
                << ",\"trailing_frames\":" << timing.mRemainderFrames << "}\n";
    std::ofstream compressed(prefix + ".packets", std::ios::binary), index(prefix + ".packet_index.jsonl");
    std::vector<unsigned char> buffer(1024 * 1024);
    SInt64 offset = 0;
    UInt64 bytes_written = 0;
    for (;;) {
        UInt32 bytes = buffer.size(), packets = 32;
        AudioStreamPacketDescription descriptions[32]{};
        const OSStatus result =
            AudioFileReadPacketData(file, false, &bytes, descriptions, offset, &packets, buffer.data());
        if (result != noErr && result != kAudioFileEndOfFileError) {
            status("file_packets", result);
            return 1;
        }
        for (UInt32 i = 0; i < packets; ++i) {
            const auto& p = descriptions[i];
            if (p.mStartOffset < 0 || static_cast<UInt64>(p.mStartOffset) + p.mDataByteSize > bytes)
                return 1;
            compressed.write(reinterpret_cast<const char*>(buffer.data() + p.mStartOffset), p.mDataByteSize);
            index << "{\"offset\":" << bytes_written << ",\"bytes\":" << p.mDataByteSize
                  << ",\"frames\":" << p.mVariableFramesInPacket << "}\n";
            bytes_written += p.mDataByteSize;
        }
        offset += packets;
        if (!packets || result == kAudioFileEndOfFileError)
            break;
    }
    AudioFileClose(file);
    std::printf("{\"stage\":\"imported\",\"packets\":%lld,\"bytes\":%llu,\"channels\":%u,\"valid_frames\":%lld}\n",
                offset,
                bytes_written,
                format.mChannelsPerFrame,
                timing.mNumberValidFrames);
    return offset && compressed.good() && index.good() && cookie_file.good() && format_file.good() && timing_file.good()
               ? 0
               : 1;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::strcmp(argv[1], "import") == 0)
        return import_file(argv[2], argv[3]);
    if (argc == 5 && std::strcmp(argv[1], "decode") == 0)
        return decode(argv[2], argv[3], std::strtoul(argv[4], nullptr, 10));
    const bool encode = argc == 9 && std::strcmp(argv[1], "encode") == 0;
    const bool properties = argc >= 3 && std::strcmp(argv[1], "properties") == 0;
    if (!encode && !properties) {
        std::fprintf(stderr,
                     "usage: codec_probe properties OUTPUT.plist [INPUT.plist] [custom:0|1]\n"
                     "       codec_probe encode SETTINGS.plist PCM.f32 INPUT_CH OUTPUT_CH PREFIX CUSTOM BITRATE\n"
                     "       codec_probe decode PREFIX OUTPUT.f32 OUTPUT_CH\n"
                     "       codec_probe import INPUT.caf_or_mp4 PREFIX\n");
        return 2;
    }
    AudioComponentDescription component_description{};
    component_description.componentType = kAudioEncoderComponentType;
    component_description.componentSubType = apac_format;
    component_description.componentManufacturer = kAudioUnitManufacturer_Apple;
    auto component = AudioComponentFindNext(nullptr, &component_description);
    AudioCodec codec = nullptr;
    if (!component || !status("create", AudioComponentInstanceNew(component, &codec)))
        return 1;
    if ((encode && std::strcmp(argv[7], "1") == 0) || (!encode && argc > 4 && std::strcmp(argv[4], "1") == 0)) {
        UInt32 one = 1;
        if (!status("cumo", AudioCodecSetProperty(codec, custom_mode, sizeof(one), &one)))
            return 1;
    }
    AudioStreamBasicDescription input{};
    input.mSampleRate = 48000;
    input.mFormatID = kAudioFormatLinearPCM;
    input.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    const UInt32 input_channels = encode ? std::strtoul(argv[4], nullptr, 10) : 1;
    const UInt32 output_channels = encode ? std::strtoul(argv[5], nullptr, 10) : 1;
    if (!input_channels || input_channels > 256 || !output_channels || output_channels > 256)
        return 2;
    input.mBytesPerPacket = input.mBytesPerFrame = 4 * input_channels;
    input.mFramesPerPacket = 1;
    input.mChannelsPerFrame = input_channels;
    input.mBitsPerChannel = 32;
    AudioStreamBasicDescription output{};
    output.mSampleRate = 48000;
    output.mFormatID = apac_format;
    output.mChannelsPerFrame = output_channels;
    if (!status("ifmt", AudioCodecSetProperty(codec, kAudioCodecPropertyCurrentInputFormat, sizeof(input), &input)) ||
        !status("ofmt", AudioCodecSetProperty(codec, kAudioCodecPropertyCurrentOutputFormat, sizeof(output), &output)))
        return 1;
    const char* settings_path = encode ? argv[2] : (argc > 3 ? argv[3] : "-");
    if (std::strcmp(settings_path, "-") != 0) {
        auto settings = read_plist(settings_path);
        if (!settings || CFGetTypeID(settings) != CFDictionaryGetTypeID())
            return 2;
        const auto success = status("set_acs", AudioCodecSetProperty(codec, asc_settings, sizeof(settings), &settings));
        CFRelease(settings);
        if (!success)
            return 1;
    }
    if (encode) {
        for (const char* suffix :
             {".asbd", ".cookie", ".timing.json", ".packets", ".packet_index.jsonl", ".settings.plist"}) {
            if (std::filesystem::exists(std::string(argv[6]) + suffix))
                return 2;
        }
        // Optional reader-selection diagnostic; this does not provide a binary
        // metadata submission API. All positive fixtures use the PCM transport.
        const char* binary_metadata = std::getenv("APAC_BINARY_METADATA");
        if (binary_metadata && std::strcmp(binary_metadata, "1") == 0) {
            const UInt32 one = 1;
            if (!status("mdpf", AudioCodecSetProperty(codec, 0x6d647066U, sizeof(one), &one)))
                return 1;
        }
        const UInt32 bitrate = std::strtoul(argv[8], nullptr, 10);
        const UInt32 source = 7, drc = 0, sync = 75;
        if (!status("brat", AudioCodecSetProperty(codec, kAudioCodecPropertyCurrentTargetBitRate, 4, &bitrate)) ||
            !status("csrc", AudioCodecSetProperty(codec, 0x63737263U, 4, &source)) ||
            !status("cdrc", AudioCodecSetProperty(codec, 0x63647263U, 4, &drc)) ||
            !status("aspf", AudioCodecSetProperty(codec, 0x61737066U, 4, &sync)) ||
            !status("initialize", AudioCodecInitialize(codec, nullptr, nullptr, nullptr, 0)))
            return 1;
        UInt32 initialized = 0, property_size = sizeof(initialized);
        status("is_initialized",
               AudioCodecGetProperty(codec, kAudioCodecPropertyIsInitialized, &property_size, &initialized));
        std::printf("{\"stage\":\"init_value\",\"initialized\":%u}\n", initialized);
        std::ifstream source_file(argv[3], std::ios::binary);
        if (!source_file)
            return 2;
        const std::string prefix = argv[6];
        std::ofstream compressed(prefix + ".packets", std::ios::binary);
        std::ofstream packet_index(prefix + ".packet_index.jsonl");
        if (!compressed || !packet_index)
            return 2;
        std::vector<float> pcm(static_cast<std::size_t>(input_channels) * 4096);
        std::vector<unsigned char> buffer(1024 * 1024);
        UInt64 total_packets = 0, total_bytes = 0, total_input = 0;
        bool eof = false;
        unsigned idle = 0;
        UInt32 loaded_bytes = 0, input_offset = 0;
        while (idle < 4) {
            UInt32 consumed_frames = 0;
            if (!eof) {
                if (input_offset == loaded_bytes) {
                    source_file.read(reinterpret_cast<char*>(pcm.data()), pcm.size() * sizeof(float));
                    loaded_bytes = static_cast<UInt32>(source_file.gcount());
                    input_offset = 0;
                }
                UInt32 bytes = loaded_bytes - input_offset;
                if (bytes % input.mBytesPerFrame)
                    return 2;
                UInt32 frames = bytes / input.mBytesPerFrame;
                if (!bytes)
                    eof = true;
                UInt32 consumed = bytes;
                consumed_frames = frames;
                if (!status(eof ? "eof" : "append",
                            AudioCodecAppendInputData(codec,
                                                      reinterpret_cast<unsigned char*>(pcm.data()) + input_offset,
                                                      &consumed,
                                                      &consumed_frames,
                                                      nullptr)))
                    return 1;
                total_input += consumed_frames;
                input_offset += consumed;
            } else {
                UInt32 bytes = 0, frames = 0;
                if (!status("eof", AudioCodecAppendInputData(codec, pcm.data(), &bytes, &frames, nullptr)))
                    return 1;
            }
            UInt32 capacity = buffer.size(), packets = 32, result = 0;
            AudioStreamPacketDescription descriptions[32]{};
            if (!status(
                    "produce",
                    AudioCodecProduceOutputPackets(codec, buffer.data(), &capacity, &packets, descriptions, &result)))
                return 1;
            std::printf(
                "{\"stage\":\"output\",\"bytes\":%u,\"packets\":%u,\"result\":%u}\n", capacity, packets, result);
            for (UInt32 i = 0; i < packets; ++i) {
                const auto& p = descriptions[i];
                if (p.mStartOffset < 0 || static_cast<UInt64>(p.mStartOffset) + p.mDataByteSize > capacity)
                    return 1;
                compressed.write(reinterpret_cast<const char*>(buffer.data() + p.mStartOffset), p.mDataByteSize);
                packet_index << "{\"offset\":" << total_bytes << ",\"bytes\":" << p.mDataByteSize
                             << ",\"frames\":" << p.mVariableFramesInPacket << "}\n";
                total_bytes += p.mDataByteSize;
                ++total_packets;
            }
            idle = packets || consumed_frames ? 0 : idle + 1;
            if (eof && result == kAudioCodecProduceOutputPacketAtEOF)
                break;
        }
        if (!eof || idle == 4) {
            std::fprintf(stderr, "encoder stopped making progress\n");
            return 1;
        }
        UInt32 cookie_size = 0;
        AudioCodecPrimeInfo prime{};
        property_size = sizeof(prime);
        if (status("encoder_prime",
                   AudioCodecGetProperty(codec, kAudioCodecPropertyPrimeInfo, &property_size, &prime))) {
            std::printf("{\"stage\":\"encoder_prime_value\",\"leading\":%u,\"trailing\":%u}\n",
                        prime.leadingFrames,
                        prime.trailingFrames);
        }
        std::ofstream timing_file(prefix + ".timing.json");
        timing_file << "{\"valid_frames\":" << total_input << ",\"leading_frames\":" << prime.leadingFrames
                    << ",\"trailing_frames\":" << prime.trailingFrames << "}\n";
        if (!status("cookie_info",
                    AudioCodecGetPropertyInfo(codec, kAudioCodecPropertyMagicCookie, &cookie_size, nullptr)))
            return 1;
        std::vector<unsigned char> cookie(cookie_size);
        if (!status("cookie",
                    AudioCodecGetProperty(codec, kAudioCodecPropertyMagicCookie, &cookie_size, cookie.data())))
            return 1;
        std::ofstream cookie_file(prefix + ".cookie", std::ios::binary);
        cookie_file.write(reinterpret_cast<const char*>(cookie.data()), cookie_size);
        property_size = sizeof(output);
        if (!status("final_ofmt",
                    AudioCodecGetProperty(codec, kAudioCodecPropertyCurrentOutputFormat, &property_size, &output)))
            return 1;
        std::ofstream format_file(prefix + ".asbd", std::ios::binary);
        format_file.write(reinterpret_cast<const char*>(&output), sizeof(output));
        CFPropertyListRef settings = nullptr;
        property_size = sizeof(settings);
        if (status("get_acs", AudioCodecGetProperty(codec, asc_settings, &property_size, &settings)) && settings) {
            write_plist(settings, (prefix + ".settings.plist").c_str());
            CFRelease(settings);
        }
        std::printf("{\"stage\":\"encoded\",\"input_frames\":%llu,\"packets\":%llu,\"bytes\":%llu,\"channels\":%u,"
                    "\"frames_per_packet\":%u}\n",
                    total_input,
                    total_packets,
                    total_bytes,
                    output.mChannelsPerFrame,
                    output.mFramesPerPacket);
        AudioCodecUninitialize(codec);
        AudioComponentInstanceDispose(codec);
        return total_packets && compressed.good() && packet_index.good() && cookie_file.good() && format_file.good()
                   ? 0
                   : 1;
    }
    UInt32 size = 0;
    Boolean writable = false;
    if (!status("acs_info", AudioCodecGetPropertyInfo(codec, asc_settings, &size, &writable)))
        return 1;
    std::printf("{\"stage\":\"acs_info_value\",\"size\":%u,\"writable\":%u}\n", size, writable);
    CFPropertyListRef settings = nullptr;
    size = sizeof(settings);
    if (!status("get_acs", AudioCodecGetProperty(codec, asc_settings, &size, &settings)))
        return 1;
    const bool saved = settings && write_plist(settings, argv[2]);
    if (settings)
        CFRelease(settings);
    AudioComponentInstanceDispose(codec);
    return saved ? 0 : 1;
}
