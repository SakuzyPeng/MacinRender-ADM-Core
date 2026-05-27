#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "adm/audio_io.h"
#include "adm/errors.h"

namespace {

class FileGuard {
  public:
    explicit FileGuard(std::filesystem::path path) : path_(std::move(path)) {}
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    FileGuard(FileGuard&&) = delete;
    FileGuard& operator=(FileGuard&&) = delete;
    ~FileGuard() { std::filesystem::remove(path_); }

  private:
    std::filesystem::path path_;
};

struct Obu {
    uint8_t type{};
    std::vector<uint8_t> payload;
};

bool check(bool condition, const char* msg) {
    if (!condition) {
        std::cerr << "FAIL: " << msg << "\n";
    }
    return condition;
}

std::vector<uint8_t> read_binary(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::optional<uint64_t> read_uleb128(const std::vector<uint8_t>& data, std::size_t& pos) {
    uint64_t value = 0;
    unsigned shift = 0;
    while (pos < data.size() && shift < 64U) {
        const uint8_t byte = data[pos++];
        value |= static_cast<uint64_t>(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0U) {
            return value;
        }
        shift += 7U;
    }
    return std::nullopt;
}

std::vector<Obu> parse_obus(const std::vector<uint8_t>& data) {
    std::vector<Obu> obus;
    std::size_t pos = 0;
    while (pos < data.size()) {
        const uint8_t header = data[pos++];
        auto size = read_uleb128(data, pos);
        if (!size || *size > data.size() - pos) {
            break;
        }
        Obu obu;
        obu.type = static_cast<uint8_t>(header >> 3U);
        obu.payload.assign(data.begin() + static_cast<std::ptrdiff_t>(pos),
                           data.begin() + static_cast<std::ptrdiff_t>(pos + *size));
        obus.push_back(std::move(obu));
        pos += static_cast<std::size_t>(*size);
    }
    return obus;
}

const Obu* find_obu(const std::vector<Obu>& obus, uint8_t type) {
    const auto it = std::ranges::find_if(obus, [type](const Obu& obu) { return obu.type == type; });
    return it == obus.end() ? nullptr : &*it;
}

std::vector<float> make_samples(uint32_t channels, uint32_t frames) {
    std::vector<float> samples(static_cast<std::size_t>(channels) * frames);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        for (uint32_t ch = 0; ch < channels; ++ch) {
            const auto index = (static_cast<std::size_t>(frame) * channels) + ch;
            samples[index] = 0.01F * static_cast<float>((ch % 7U) + 1U);
        }
    }
    return samples;
}

std::vector<Obu> write_iamf(uint32_t channels, const std::string& layout_id, const std::string& path) {
    constexpr uint32_t k_sample_rate = 48000U;
    constexpr uint32_t k_frames = 960U;

    auto writer = mradm::audio::FloatIamfWriter::open(path, channels, k_sample_rate, 0U, layout_id);
    if (!check(writer.has_value(), "FloatIamfWriter::open failed")) {
        return {};
    }

    auto samples = make_samples(channels, k_frames);
    if (!check(writer->write(samples.data(), k_frames) == k_frames, "IAMF short write")) {
        return {};
    }
    if (!check(writer->close().has_value(), "IAMF close failed")) {
        return {};
    }

    return parse_obus(read_binary(path));
}

bool verify_916_uses_simple_profile() {
    const std::string path = "/tmp/mr_iamf_916_profile.iamf";
    const FileGuard guard(path);

    const auto obus = write_iamf(16U, "9.1.6", path);
    const Obu* seq = find_obu(obus, 31U);
    if (!check(seq != nullptr, "9.1.6 sequence header OBU present")) {
        return false;
    }

    const std::array<uint8_t, 6> expected{'i', 'a', 'm', 'f', 0U, 0U};
    return check(seq->payload.size() == expected.size(), "9.1.6 sequence header size") &&
           check(std::ranges::equal(seq->payload, expected), "9.1.6 uses Simple Profile");
}

bool verify_222_is_rejected_without_nonstandard_profile() {
    const std::string path = "/tmp/mr_iamf_222_rejected.iamf";
    const FileGuard guard(path);

    auto writer = mradm::audio::FloatIamfWriter::open(path, 24U, 48000U, 0U, "9+10+3");
    if (!check(!writer.has_value(), "22.2 IAMF open is rejected")) {
        return false;
    }
    bool ok = true;
    ok &= check(writer.error().code == mradm::ErrorCode::unsupported, "22.2 IAMF returns unsupported");
    ok &= check(writer.error().message.find("public IAMF v1.1") != std::string::npos,
                "22.2 IAMF explains public profile limitation");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= verify_916_uses_simple_profile();
    ok &= verify_222_is_rejected_without_nonstandard_profile();
    return ok ? 0 : 1;
}
