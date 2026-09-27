// Synchronous system Atmos decoding through AudioFile + AudioConverter.
// No AVPlayer, AudioQueue, output device, run loop, or real-time clock is used.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <string>
#include <vector>

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

namespace {
constexpr UInt32 joc = 0x65632b33U; // ec+3, selected only when returned in the file's format list.

bool check(const char* stage, OSStatus status) {
    std::printf("{\"stage\":\"%s\",\"status\":%d}\n", stage, status);
    std::fflush(stdout);
    return status == noErr;
}

void format(const char* stage, const AudioStreamBasicDescription& value, UInt32 tag = 0) {
    char code[5]{
        char(value.mFormatID >> 24), char(value.mFormatID >> 16), char(value.mFormatID >> 8), char(value.mFormatID), 0};
    std::printf("{\"stage\":\"%s\",\"format\":\"%s\",\"rate\":%.0f,\"channels\":%u,"
                "\"flags\":%u,\"frames_per_packet\":%u,\"layout_tag\":%u}\n",
                stage,
                code,
                value.mSampleRate,
                value.mChannelsPerFrame,
                value.mFormatFlags,
                value.mFramesPerPacket,
                tag);
}

struct Resources {
    AudioFileID input{}, output{};
    AudioConverterRef converter{};
    ~Resources() {
        if (converter)
            AudioConverterDispose(converter);
        if (input)
            AudioFileClose(input);
        if (output)
            AudioFileClose(output);
    }
};

struct Feed {
    AudioFileID file{};
    SInt64 packet{};
    UInt32 channels{};
    std::vector<unsigned char> bytes;
    std::vector<AudioStreamPacketDescription> descriptions = std::vector<AudioStreamPacketDescription>(32);
};

OSStatus input(AudioConverterRef,
               UInt32* packets,
               AudioBufferList* buffers,
               AudioStreamPacketDescription** descriptions,
               void* context) {
    auto& feed = *static_cast<Feed*>(context);
    *packets = std::min(*packets, UInt32(feed.descriptions.size()));
    UInt32 bytes = static_cast<UInt32>(feed.bytes.size());
    const auto status = AudioFileReadPacketData(
        feed.file, false, &bytes, feed.descriptions.data(), feed.packet, packets, feed.bytes.data());
    if (status != noErr && status != kAudioFileEndOfFileError)
        return status;
    feed.packet += *packets;
    buffers->mNumberBuffers = 1;
    buffers->mBuffers[0] = {*packets ? feed.channels : 0U, bytes, feed.bytes.data()};
    if (descriptions)
        *descriptions = feed.descriptions.data();
    return noErr;
}

void inspect_layouts(AudioConverterRef converter) {
    UInt32 size = 0;
    const auto property = kAudioCodecPropertyAvailableOutputChannelLayoutTags;
    if (!check("available_layouts_info", AudioConverterGetPropertyInfo(converter, property, &size, nullptr)) ||
        size > 65536 || size % sizeof(UInt32))
        return;
    std::vector<UInt32> tags(size / sizeof(UInt32));
    if (!check("available_layouts", AudioConverterGetProperty(converter, property, &size, tags.data())))
        return;
    std::printf("{\"stage\":\"available_layout_values\",\"tags\":[");
    for (size_t i = 0; i < tags.size(); ++i)
        std::printf("%s%u", i ? "," : "", tags[i]);
    std::puts("]}");
}
} // namespace

int main(int argc, const char** argv) {
    if (argc != 7) {
        std::fprintf(stderr, "usage: offline_probe INPUT OUTPUT.caf|- LAYOUT_TAG base|joc FIRST_SECONDS END_SECONDS\n");
        return 2;
    }
    const UInt32 tag = std::strtoul(argv[3], nullptr, 0);
    const UInt32 channels = tag & 0xFFFFU;
    const double first = std::strtod(argv[5], nullptr), end = std::strtod(argv[6], nullptr);
    if (!channels || channels > 32 || first < 0 || end <= first || end > 150 ||
        (std::strcmp(argv[4], "base") && std::strcmp(argv[4], "joc")))
        return 2;
    const bool write = std::strcmp(argv[2], "-") != 0;
    if (write && std::filesystem::exists(argv[2]))
        return 2;
    const bool want_joc = std::strcmp(argv[4], "joc") == 0;
    constexpr std::array<UInt32, 6> verified_layouts{kAudioChannelLayoutTag_MPEG_7_1_C,
                                                     kAudioChannelLayoutTag_Atmos_5_1_2,
                                                     kAudioChannelLayoutTag_Atmos_5_1_4,
                                                     kAudioChannelLayoutTag_Atmos_7_1_2,
                                                     kAudioChannelLayoutTag_Atmos_7_1_4,
                                                     kAudioChannelLayoutTag_Atmos_9_1_6};
    if (want_joc && std::find(verified_layouts.begin(), verified_layouts.end(), tag) == verified_layouts.end() &&
        !std::getenv("ATMOS_ALLOW_UNVERIFIED_LAYOUT")) {
        std::fprintf(stderr, "Unverified Atmos output layout; API acceptance alone can conceal missing channels.\n");
        return 2;
    }
    Resources resources;
    if (std::getenv("ATMOS_REGISTER_JOC")) {
        // Reproduce the process-local registration observed in AVPlayer on this
        // OS. The factory export is private and is not a supported product API.
        void* image = dlopen("/System/Library/Components/AudioCodecs.component/Contents/MacOS/AudioCodecs",
                             RTLD_NOW | RTLD_LOCAL);
        auto factory =
            image ? reinterpret_cast<AudioComponentFactoryFunction>(dlsym(image, "ACAC3DecoderNewFactory")) : nullptr;
        const AudioComponentDescription component{
            kAudioDecoderComponentType, joc, kAudioUnitManufacturer_Apple, kAudioComponentFlag_SandboxSafe, 0};
        if (!factory || !AudioComponentRegister(&component, CFSTR("Local Atmos decoder research"), 0, factory)) {
            std::fprintf(stderr, "Observed Atmos decoder factory is unavailable.\n");
            return 1;
        }
        std::puts(
            "{\"stage\":\"process_local_registration\",\"factory\":\"ACAC3DecoderNewFactory\",\"subtype\":\"ec+3\"}");
    }
    auto url = CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8*>(argv[1]), std::strlen(argv[1]), false);
    const auto opened = AudioFileOpenURL(url, kAudioFileReadPermission, 0, &resources.input);
    CFRelease(url);
    if (!check("open", opened))
        return 1;
    AudioStreamBasicDescription source{};
    UInt32 size = sizeof(source);
    if (!check("file_format", AudioFileGetProperty(resources.input, kAudioFilePropertyDataFormat, &size, &source)))
        return 1;
    format("file_format_value", source);
    bool found_joc = false;
    if (check("format_list_info",
              AudioFileGetPropertyInfo(resources.input, kAudioFilePropertyFormatList, &size, nullptr)) &&
        size % sizeof(AudioFormatListItem) == 0 && size < 65536) {
        std::vector<AudioFormatListItem> list(size / sizeof(AudioFormatListItem));
        if (check("format_list",
                  AudioFileGetProperty(resources.input, kAudioFilePropertyFormatList, &size, list.data()))) {
            list.resize(size / sizeof(AudioFormatListItem));
            for (const auto& item : list) {
                format("format_list_value", item.mASBD, item.mChannelLayoutTag);
                if (item.mASBD.mFormatID == joc) {
                    if (want_joc && !found_joc)
                        source = item.mASBD;
                    found_joc = true;
                }
            }
        }
    }
    if (want_joc && !found_joc) {
        std::fprintf(stderr, "File does not advertise an ec+3 format; refusing a forced reinterpretation.\n");
        return 1;
    }
    std::vector<unsigned char> cookie;
    if (AudioFileGetPropertyInfo(resources.input, kAudioFilePropertyMagicCookieData, &size, nullptr) == noErr &&
        size > 0 && size <= 65536) {
        cookie.resize(size);
        if (!check("cookie",
                   AudioFileGetProperty(resources.input, kAudioFilePropertyMagicCookieData, &size, cookie.data())))
            return 1;
    }
    AudioFilePacketTableInfo timing{};
    size = sizeof(timing);
    if (check("packet_table", AudioFileGetProperty(resources.input, kAudioFilePropertyPacketTableInfo, &size, &timing)))
        std::printf("{\"stage\":\"timing\",\"valid\":%lld,\"priming\":%d,\"remainder\":%d}\n",
                    timing.mNumberValidFrames,
                    timing.mPrimingFrames,
                    timing.mRemainderFrames);
    AudioStreamBasicDescription pcm{};
    pcm.mSampleRate = source.mSampleRate;
    pcm.mFormatID = kAudioFormatLinearPCM;
    pcm.mFormatFlags = UInt32(kAudioFormatFlagsNativeFloatPacked) | UInt32(kAudioFormatFlagIsNonInterleaved);
    pcm.mBytesPerFrame = pcm.mBytesPerPacket = sizeof(float);
    pcm.mFramesPerPacket = 1;
    pcm.mChannelsPerFrame = channels;
    pcm.mBitsPerChannel = 32;
    format("selected_input", source);
    format("requested_output", pcm, tag);
    OSStatus created_converter = noErr;
    if (const char* selector = std::getenv("ATMOS_CODEC_SELECTOR")) {
        const AudioClassDescription codec{
            kAudioDecoderComponentType, std::strcmp(selector, "base") == 0 ? kAudioFormatEnhancedAC3 : joc, 0};
        created_converter = AudioConverterNewSpecific(&source, &pcm, 1, &codec, &resources.converter);
    } else {
        created_converter = AudioConverterNew(&source, &pcm, &resources.converter);
    }
    if (!check("converter_new", created_converter))
        return 1;
    if (!cookie.empty() && !check("converter_cookie",
                                  AudioConverterSetProperty(resources.converter,
                                                            kAudioConverterDecompressionMagicCookie,
                                                            static_cast<UInt32>(cookie.size()),
                                                            cookie.data())))
        return 1;
    inspect_layouts(resources.converter);
    AudioChannelLayout layout{};
    layout.mChannelLayoutTag = tag;
    if (!check("output_layout",
               AudioConverterSetProperty(
                   resources.converter, kAudioConverterOutputChannelLayout, sizeof(layout), &layout)))
        return 1;
    // Match the observed AVPlayer converter's zero explicit priming request.
    AudioConverterPrimeInfo prime{};
    check("prime", AudioConverterSetProperty(resources.converter, kAudioConverterPrimeInfo, sizeof(prime), &prime));
    size = sizeof(pcm);
    if (!check(
            "actual_output",
            AudioConverterGetProperty(resources.converter, kAudioConverterCurrentOutputStreamDescription, &size, &pcm)))
        return 1;
    format("actual_output_value", pcm);
    if (pcm.mChannelsPerFrame != channels || pcm.mBitsPerChannel != 32 ||
        !(pcm.mFormatFlags & kAudioFormatFlagIsFloat) || !(pcm.mFormatFlags & kAudioFormatFlagIsNonInterleaved))
        return 1;
    size = sizeof(layout);
    if (!check("actual_layout",
               AudioConverterGetProperty(resources.converter, kAudioConverterOutputChannelLayout, &size, &layout)))
        return 1;
    std::printf("{\"stage\":\"actual_layout_value\",\"tag\":%u}\n", layout.mChannelLayoutTag);
    if (layout.mChannelLayoutTag != tag)
        return 1;

    if (write) {
        auto output = pcm;
        output.mFormatFlags &= ~kAudioFormatFlagIsNonInterleaved;
        output.mBytesPerPacket = output.mBytesPerFrame = sizeof(float) * channels;
        url = CFURLCreateFromFileSystemRepresentation(
            nullptr, reinterpret_cast<const UInt8*>(argv[2]), std::strlen(argv[2]), false);
        const auto created = AudioFileCreateWithURL(url, kAudioFileCAFType, &output, 0, &resources.output);
        CFRelease(url);
        if (!check("create_output", created) ||
            !check("write_layout",
                   AudioFileSetProperty(resources.output, kAudioFilePropertyChannelLayout, sizeof(layout), &layout)))
            return 1;
    }
    UInt32 packet_size = 0;
    size = sizeof(packet_size);
    if (!check("packet_size",
               AudioFileGetProperty(resources.input, kAudioFilePropertyPacketSizeUpperBound, &size, &packet_size)) ||
        !packet_size || packet_size > 1048576)
        return 1;
    Feed feed;
    feed.file = resources.input;
    feed.channels = source.mChannelsPerFrame;
    feed.bytes.resize(size_t(packet_size) * 32);
    constexpr UInt32 capacity = 4096;
    std::vector<unsigned char> buffer_storage(offsetof(AudioBufferList, mBuffers) + channels * sizeof(AudioBuffer));
    auto* buffers = reinterpret_cast<AudioBufferList*>(buffer_storage.data());
    std::vector<float> planes(size_t(channels) * capacity), interleaved(size_t(channels) * capacity);
    std::vector<double> energies(channels, 0.0), peaks(channels, 0.0);
    const uint64_t first_frame = std::llround(first * pcm.mSampleRate), end_frame = std::llround(end * pcm.mSampleRate);
    uint64_t total = 0, retained = 0;
    bool finite = true;
    const auto begin = std::chrono::steady_clock::now();
    while (total < end_frame) {
        buffers->mNumberBuffers = channels;
        for (UInt32 c = 0; c < channels; ++c)
            buffers->mBuffers[c] = {1, capacity * sizeof(float), planes.data() + size_t(c) * capacity};
        UInt32 frames = capacity;
        const auto status =
            AudioConverterFillComplexBuffer(resources.converter, input, &feed, &frames, buffers, nullptr);
        if (status != noErr) {
            check("fill_error", status);
            return 1;
        }
        if (!frames)
            break;
        if (frames > capacity)
            return 1;
        const auto low = std::max(first_frame, total), high = std::min(end_frame, total + frames);
        if (low < high) {
            for (uint64_t f = low; f < high; ++f)
                for (UInt32 c = 0; c < channels; ++c) {
                    const float sample = planes[size_t(c) * capacity + (f - total)];
                    finite = finite && std::isfinite(sample);
                    interleaved[(f - low) * channels + c] = sample;
                    energies[c] += double(sample) * sample;
                    peaks[c] = std::max(peaks[c], std::fabs(double(sample)));
                }
            if (write) {
                UInt32 packets = static_cast<UInt32>(high - low);
                const auto written = AudioFileWritePackets(resources.output,
                                                           false,
                                                           packets * channels * sizeof(float),
                                                           nullptr,
                                                           retained,
                                                           &packets,
                                                           interleaved.data());
                if (written || packets != high - low) {
                    check("write_error", written);
                    return 1;
                }
            }
            retained += high - low;
        }
        total += frames;
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    std::printf("{\"stage\":\"completed\",\"decoded_frames\":%llu,\"retained_frames\":%llu,\"seconds\":%.9f,"
                "\"finite\":%s,\"rms\":[",
                static_cast<unsigned long long>(total),
                static_cast<unsigned long long>(retained),
                elapsed,
                finite ? "true" : "false");
    for (UInt32 c = 0; c < channels; ++c)
        std::printf("%s%.12g", c ? "," : "", retained ? std::sqrt(energies[c] / retained) : 0.0);
    std::printf("],\"peak\":[");
    for (UInt32 c = 0; c < channels; ++c)
        std::printf("%s%.12g", c ? "," : "", peaks[c]);
    std::puts("]}");
    return finite && retained == end_frame - first_frame ? 0 : 1;
}
