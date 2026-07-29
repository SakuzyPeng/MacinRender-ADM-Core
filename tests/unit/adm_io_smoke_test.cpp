#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "adm/audio_io.h"
#include "adm/io.h"

#include "test_portable.h"

namespace {

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
    }
    return condition;
}

class FileGuard {
  public:
    explicit FileGuard(std::filesystem::path path) : path_(std::move(path)) {}
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    FileGuard(FileGuard&&) = delete;
    FileGuard& operator=(FileGuard&&) = delete;
    ~FileGuard() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

void write_u16(std::ostream& output, uint16_t value) {
    const std::array<char, 2> bytes = {static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8U) & 0xFFU)};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_u32(std::ostream& output, uint32_t value) {
    const std::array<char, 4> bytes = {static_cast<char>(value & 0xFFU),
                                       static_cast<char>((value >> 8U) & 0xFFU),
                                       static_cast<char>((value >> 16U) & 0xFFU),
                                       static_cast<char>((value >> 24U) & 0xFFU)};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

bool write_extensible_51(const std::filesystem::path& path, std::optional<std::string_view> axml = std::nullopt) {
    constexpr uint16_t k_channels = 6;
    constexpr uint32_t k_sample_rate = 48000;
    constexpr uint16_t k_bits = 16;
    constexpr uint16_t k_block_align = k_channels * (k_bits / 8U);
    constexpr uint32_t k_frames = 16;
    constexpr uint32_t k_data_size = k_frames * k_block_align;
    const uint32_t axml_padded = axml ? static_cast<uint32_t>(axml->size() + (axml->size() & 1U)) : 0U;
    const uint32_t riff_size = 4U + 8U + 40U + (axml ? 8U + axml_padded : 0U) + 8U + k_data_size;

    std::ofstream output(path, std::ios::binary);
    if (!output) {
        return false;
    }
    output.write("RIFF", 4);
    write_u32(output, riff_size);
    output.write("WAVE", 4);
    output.write("fmt ", 4);
    write_u32(output, 40U);
    write_u16(output, 0xFFFEU); // WAVE_FORMAT_EXTENSIBLE
    write_u16(output, k_channels);
    write_u32(output, k_sample_rate);
    write_u32(output, k_sample_rate * k_block_align);
    write_u16(output, k_block_align);
    write_u16(output, k_bits);
    write_u16(output, 22U);
    write_u16(output, k_bits);
    write_u32(output, 0x003FU);
    const std::array<char, 16> pcm_subformat = {1,
                                                0,
                                                0,
                                                0,
                                                0,
                                                0,
                                                0x10,
                                                0,
                                                static_cast<char>(0x80),
                                                0,
                                                0,
                                                static_cast<char>(0xAA),
                                                0,
                                                0x38,
                                                static_cast<char>(0x9B),
                                                0x71};
    output.write(pcm_subformat.data(), static_cast<std::streamsize>(pcm_subformat.size()));
    if (axml) {
        output.write("axml", 4);
        write_u32(output, static_cast<uint32_t>(axml->size()));
        output.write(axml->data(), static_cast<std::streamsize>(axml->size()));
        if ((axml->size() & 1U) != 0U) {
            output.put('\0');
        }
    }
    output.write("data", 4);
    write_u32(output, k_data_size);
    for (uint32_t frame = 0; frame < k_frames; ++frame) {
        for (uint16_t channel = 0; channel < k_channels; ++channel) {
            write_u16(output, static_cast<uint16_t>((frame + channel + 1U) * 100U));
        }
    }
    return output.good();
}

bool verify_channel_catalog() {
    bool ok = !mradm::io::input_layouts().empty();
    const auto layout_714 =
        std::ranges::find_if(mradm::io::input_layouts(), [](const auto& layout) { return layout.id == "7.1.4"; });
    ok = check(layout_714 != mradm::io::input_layouts().end() && layout_714->channels[4].token == "M+135" &&
                   layout_714->channels[6].token == "M+090" && layout_714->wave_mask_channels[4].token == "M+135" &&
                   layout_714->wave_mask_channels[6].token == "M+090",
               "7.1.4 explicit and automatic file order both follow WAVE mask order") &&
         ok;
    auto aliases = mradm::io::resolve_input_channel_labels({"L", "R", "C", "LFE"});
    ok = check(aliases.has_value() && aliases->size() == 4U, "channel aliases resolve") && ok;
    if (aliases) {
        ok = check(aliases->at(0).speaker_label == "M+030" && aliases->at(0).azimuth == 30.0F,
                   "L is M+030 at +30 degrees") &&
             ok;
        ok = check(aliases->at(1).speaker_label == "M-030" && aliases->at(1).azimuth == -30.0F,
                   "R is M-030 at -30 degrees") &&
             ok;
        ok = check(aliases->at(3).is_lfe, "LFE alias has LFE semantics") && ok;
    }
    ok = check(!mradm::io::resolve_input_channel_labels({"U+110"}).has_value(), "ambiguous U+110 requires elevation") &&
         ok;
    auto qualified = mradm::io::resolve_input_channel_labels({"U+110@30", "U-110@45"});
    ok = check(qualified.has_value() && qualified->at(0).elevation == 30.0F && qualified->at(1).elevation == 45.0F,
               "qualified U labels select nominal elevation") &&
         ok;
    ok = check(!mradm::io::resolve_input_channel_labels({"L", "M+030"}).has_value(),
               "duplicate canonical position rejected") &&
         ok;
    ok = check(!mradm::io::resolve_input_channel_labels({"L", "", "R"}).has_value(),
               "empty custom channel label rejected") &&
         ok;
    return ok;
}

bool verify_custom_float_channel_bed() {
    FileGuard file{mr_test::temp_prefix() + "channel_bed_float.wav"};
    std::vector<float> samples(static_cast<std::size_t>(4U) * 16U, 0.0F);
    samples[0] = 1.0F;
    samples[5] = 1.0F;
    samples[10] = 1.0F;
    samples[15] = 0.5F;
    {
        auto writer = mradm::audio::FloatWavWriter::open(file.path().string(), 4U, 48000U);
        if (!check(writer.has_value(), "create float channel-bed fixture")) {
            return false;
        }
        if (!check(writer->write(samples.data(), 16U) == 16U, "write float channel-bed fixture")) {
            return false;
        }
    }

    mradm::io::SceneImportOptions options;
    options.input_channel_labels = {"L", "R", "C", "LFE"};
    auto scene = mradm::io::import_scene(file.path().string(), options);
    bool ok = check(scene.has_value(), "import custom float channel bed");
    if (!scene) {
        std::cerr << scene.error().message << "\n";
        return false;
    }
    ok = check(scene->info.source_kind == mradm::SceneSourceKind::channel_bed, "source kind is channel bed") && ok;
    ok = check(scene->info.input_layout == "custom" && scene->info.num_channels == 4U, "custom layout metadata") && ok;
    ok = check(scene->objects.size() == 1U && scene->objects[0].tracks.size() == 4U,
               "one synthetic object with one track per channel") &&
         ok;
    const auto& left = scene->objects[0].tracks[0].ds_blocks[0];
    const auto& lfe = scene->objects[0].tracks[3].ds_blocks[0];
    ok = check(left.has_position && left.azimuth == 30.0F && left.elevation == 0.0F,
               "synthetic L block carries exact geometry") &&
         ok;
    ok = check(!lfe.has_position && lfe.low_pass_hz.has_value(), "synthetic LFE carries semantic marker only") && ok;

    mradm::io::SceneImportOptions wrong;
    wrong.input_layout = "5.1";
    ok = check(!mradm::io::import_scene(file.path().string(), wrong).has_value(),
               "preset/file channel-count mismatch rejected") &&
         ok;
    return ok;
}

bool verify_auto_mask_and_axml_precedence() {
    FileGuard plain{mr_test::temp_prefix() + "channel_bed_extensible_51.wav"};
    if (!check(write_extensible_51(plain.path()), "create extensible 5.1 fixture")) {
        return false;
    }
    auto scene = mradm::io::import_scene(plain.path().string(), {});
    bool ok = check(scene.has_value(), "auto-detect 5.1 channel mask");
    if (scene) {
        ok = check(scene->info.source_kind == mradm::SceneSourceKind::channel_bed &&
                       scene->info.input_layout == "5.1" && scene->info.input_channel_labels.size() == 6U,
                   "auto mask resolves exact 5.1 input mapping") &&
             ok;
    }

    FileGuard malformed{mr_test::temp_prefix() + "channel_bed_malformed_axml.wav"};
    if (!check(write_extensible_51(malformed.path(), "<"), "create malformed AXML fixture")) {
        return false;
    }
    ok = check(!mradm::io::import_scene(malformed.path().string(), {}).has_value(),
               "present but malformed AXML never falls back to channel mask") &&
         ok;

    mradm::io::SceneImportOptions forced;
    forced.input_layout = "5.1";
    auto forced_scene = mradm::io::import_scene(malformed.path().string(), forced);
    ok = check(forced_scene.has_value(), "explicit input layout ignores embedded AXML") && ok;
    if (forced_scene) {
        ok = check(!forced_scene->import_warnings.empty(), "ignored AXML produces an import warning") && ok;
    }
    return ok;
}

} // namespace

int main() {
    // Non-existent file must return io_error (not throw)
    auto result = mradm::io::import_scene(mr_test::temp_prefix() + "nonexistent_adm_mr_test_xyz.wav");

    if (result.has_value()) {
        std::cerr << "expected error for nonexistent file, got success\n";
        return EXIT_FAILURE;
    }
    if (result.error().code != mradm::ErrorCode::io_error) {
        std::cerr << "expected io_error, got code " << static_cast<int>(result.error().code) << "\n";
        return EXIT_FAILURE;
    }
    if (result.error().message.empty()) {
        std::cerr << "error message must not be empty\n";
        return EXIT_FAILURE;
    }

    bool ok = verify_channel_catalog();
    ok = verify_custom_float_channel_bed() && ok;
    ok = verify_auto_mask_and_axml_precedence() && ok;
    std::cout << "adm_io smoke test: " << result.error().message << "\n";
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
