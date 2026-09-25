// Read decoder-reported metadata/renderer descriptors without changing properties.
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    const std::string prefix = argv[1], output_prefix = argv[2];
    AudioStreamBasicDescription input{};
    std::ifstream asbd(prefix + ".asbd", std::ios::binary);
    if (!asbd.read(reinterpret_cast<char*>(&input), sizeof(input)))
        return 2;
    std::ifstream cookie_file(prefix + ".cookie", std::ios::binary);
    std::vector<char> cookie((std::istreambuf_iterator<char>(cookie_file)), std::istreambuf_iterator<char>());
    AudioComponentDescription description{kAudioDecoderComponentType, 0x61706163U, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent component = AudioComponentFindNext(nullptr, &description);
    AudioCodec codec = nullptr;
    if (!component || AudioComponentInstanceNew(component, &codec))
        return 1;
    AudioStreamBasicDescription pcm{};
    pcm.mSampleRate = input.mSampleRate;
    pcm.mFormatID = kAudioFormatLinearPCM;
    pcm.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    pcm.mChannelsPerFrame = input.mChannelsPerFrame;
    pcm.mBytesPerFrame = pcm.mBytesPerPacket = pcm.mChannelsPerFrame * sizeof(float);
    pcm.mFramesPerPacket = 1;
    pcm.mBitsPerChannel = 32;
    const auto initialized = AudioCodecInitialize(codec, &input, &pcm, cookie.data(), cookie.size());
    std::printf("{\"stage\":\"initialize\",\"status\":%d}\n", initialized);
    if (initialized)
        return 1;
    for (const auto* text : {"mdpf", "mdcf", "mdfs", "imrd", "cori"}) {
        UInt32 property = 0;
        for (unsigned i = 0; i < 4; ++i)
            property = (property << 8U) | static_cast<unsigned char>(text[i]);
        UInt32 bytes = 0;
        auto status = AudioCodecGetPropertyInfo(codec, property, &bytes, nullptr);
        if (status || bytes > 1024 * 1024) {
            std::printf("{\"property\":\"%s\",\"status\":%d,\"bytes\":%u}\n", text, status, bytes);
            continue;
        }
        std::vector<unsigned char> data(bytes);
        status = AudioCodecGetProperty(codec, property, &bytes, data.data());
        std::printf("{\"property\":\"%s\",\"status\":%d,\"bytes\":%u", text, status, bytes);
        if (!status) {
            const auto path = output_prefix + "." + text;
            if (std::filesystem::exists(path))
                return 2;
            std::ofstream file(path, std::ios::binary);
            file.write(reinterpret_cast<const char*>(data.data()), bytes);
            if (!file.good())
                return 2;
            if (bytes == 4) {
                UInt32 value = 0;
                std::memcpy(&value, data.data(), 4);
                std::printf(",\"value\":%u", value);
            }
        }
        std::puts("}");
    }
    AudioCodecUninitialize(codec);
    AudioComponentInstanceDispose(codec);
    return 0;
}
