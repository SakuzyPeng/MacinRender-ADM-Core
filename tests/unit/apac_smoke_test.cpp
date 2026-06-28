#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <numbers>
#include <string>
#include <vector>

#include "adm/audio_io.h"
#include "adm/errors.h"

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "test_portable.h"

namespace {

class FileGuard {
  public:
    explicit FileGuard(std::filesystem::path p) : path_(std::move(p)) {}
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    FileGuard(FileGuard&&) = delete;
    FileGuard& operator=(FileGuard&&) = delete;
    ~FileGuard() { std::filesystem::remove(path_); }

  private:
    std::filesystem::path path_;
};

bool check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
    }
    return cond;
}

struct ProgressSnapshot {
    mradm::RenderStage stage{};
    mradm::RenderOperation operation{};
    double fraction{};
    double stage_fraction{};
    uint64_t current_frame{};
    uint64_t total_frames{};
};

class CapturingProgressSink final : public mradm::ProgressSink {
  public:
    void on_progress(const mradm::ProgressEvent& event) override {
        events_.push_back({event.stage,
                           event.operation,
                           event.fraction,
                           event.stage_fraction,
                           event.current_frame,
                           event.total_frames});
    }

    [[nodiscard]] const std::vector<ProgressSnapshot>& events() const noexcept { return events_; }

  private:
    std::vector<ProgressSnapshot> events_;
};

bool verify_apac_progress(const CapturingProgressSink& progress, uint64_t expected_frames) {
    const auto& events = progress.events();
    bool ok = check(events.size() > 2U, "APAC progress should include intermediate frame events");
    bool stage_ok = true;
    bool operation_ok = true;
    bool fraction_ok = true;
    bool frame_ok = true;
    bool midpoint_ok = false;
    double prev = -1.0;
    for (const auto& ev : events) {
        stage_ok = stage_ok && ev.stage == mradm::RenderStage::post_processing;
        operation_ok = operation_ok && ev.operation == mradm::RenderOperation::encode_apac;
        fraction_ok = fraction_ok && ev.fraction >= prev && ev.fraction >= 0.0 && ev.fraction <= 1.0 &&
                      ev.stage_fraction >= 0.0 && ev.stage_fraction <= 1.0;
        frame_ok = frame_ok && ev.total_frames == expected_frames && ev.current_frame <= ev.total_frames;
        midpoint_ok = midpoint_ok || (ev.current_frame > 0U && ev.current_frame < ev.total_frames);
        prev = ev.fraction;
    }
    ok = check(stage_ok, "APAC progress stage should be post_processing") && ok;
    ok = check(operation_ok, "APAC progress operation should be encode_apac") && ok;
    ok = check(fraction_ok, "APAC progress fraction should be monotonic in [0,1]") && ok;
    ok = check(frame_ok, "APAC progress frames should match input frame count") && ok;
    ok = check(midpoint_ok, "APAC progress should report a non-terminal frame position") && ok;
    return ok;
}

// Write a 1-second 48 kHz WAV with per-channel sine amplitudes taken from |amps|.
// |amps| must have exactly |channels| entries.
bool write_test_wav_with_amps(const std::string& path,
                              uint32_t channels,
                              const std::vector<float>& amps,
                              uint32_t frames = 48000U) {
    auto wr = mradm::audio::FloatWavWriter::open(path, channels, 48000U);
    if (!check(wr.has_value(), "FloatWavWriter::open failed")) {
        return false;
    }
    std::vector<float> buf(static_cast<std::size_t>(channels) * frames, 0.0F);
    constexpr float k_freq = 440.0F;
    constexpr float k_sr = 48000.0F;
    for (uint32_t f = 0; f < frames; ++f) {
        const float s = std::sin(2.0F * std::numbers::pi_v<float> * k_freq * static_cast<float>(f) / k_sr);
        for (uint32_t c = 0; c < channels; ++c) {
            buf[(static_cast<std::size_t>(f) * channels) + c] = amps[c] * s;
        }
    }
    return check(wr->write(buf.data(), frames) == frames, "WAV short write");
}

bool write_test_wav(const std::string& path, uint32_t channels, uint32_t sample_rate, uint32_t frames) {
    auto wr = mradm::audio::FloatWavWriter::open(path, channels, sample_rate);
    if (!check(wr.has_value(), "FloatWavWriter::open failed")) {
        return false;
    }
    std::vector<float> buf(static_cast<std::size_t>(channels) * frames);
    for (std::size_t i = 0; i < buf.size(); ++i) {
        buf[i] = 0.25F * std::sin(static_cast<float>(i) * 0.1F);
    }
    return check(wr->write(buf.data(), frames) == frames, "WAV short write");
}

#ifdef __APPLE__

// Decode an APAC .m4a to interleaved float32 PCM via AudioToolbox.
// Returns false on any error; out is resized to the decoded sample count.
// NOLINTNEXTLINE(readability-function-size)
bool decode_apac_to_pcm(const std::string& path, uint32_t channels, std::vector<float>& out) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr,
                                                           reinterpret_cast<const UInt8*>(path.c_str()),
                                                           static_cast<CFIndex>(path.size()),
                                                           static_cast<Boolean>(false));
    if (url == nullptr) {
        return false;
    }

    ExtAudioFileRef ext_file = nullptr;
    OSStatus err = ExtAudioFileOpenURL(url, &ext_file);
    CFRelease(url);
    if (err != noErr || ext_file == nullptr) {
        return false;
    }

    AudioStreamBasicDescription client_fmt{};
    client_fmt.mSampleRate = 48000.0;
    client_fmt.mFormatID = kAudioFormatLinearPCM;
    client_fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    client_fmt.mChannelsPerFrame = channels;
    client_fmt.mBitsPerChannel = 32;
    client_fmt.mFramesPerPacket = 1;
    client_fmt.mBytesPerFrame = static_cast<UInt32>(sizeof(float) * channels);
    client_fmt.mBytesPerPacket = client_fmt.mBytesPerFrame;

    err = ExtAudioFileSetProperty(ext_file, kExtAudioFileProperty_ClientDataFormat, sizeof(client_fmt), &client_fmt);
    if (err != noErr) {
        ExtAudioFileDispose(ext_file);
        return false;
    }

    SInt64 file_frames = 0;
    UInt32 sz = sizeof(file_frames);
    ExtAudioFileGetProperty(ext_file, kExtAudioFileProperty_FileLengthFrames, &sz, &file_frames);
    if (file_frames <= 0) {
        ExtAudioFileDispose(ext_file);
        return false;
    }

    out.resize(static_cast<std::size_t>(file_frames) * channels);
    constexpr UInt32 k_block = 4096;
    float* ptr = out.data();
    auto frames_remaining = static_cast<UInt32>(file_frames);
    uint64_t total_read = 0;

    while (frames_remaining > 0) {
        UInt32 n = std::min(k_block, frames_remaining);
        AudioBufferList abl{};
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = channels;
        abl.mBuffers[0].mDataByteSize = static_cast<UInt32>(static_cast<std::size_t>(n) * channels * sizeof(float));
        abl.mBuffers[0].mData = ptr;
        err = ExtAudioFileRead(ext_file, &n, &abl);
        if (err != noErr || n == 0) {
            break;
        }
        ptr += static_cast<std::ptrdiff_t>(n) * channels;
        frames_remaining -= n;
        total_read += n;
    }

    ExtAudioFileDispose(ext_file);
    out.resize(static_cast<std::size_t>(total_read) * channels);
    return !out.empty();
}

// Per-channel RMS over the decoded interleaved buffer.
float channel_rms(const std::vector<float>& samples, uint32_t ch, uint32_t channels) {
    double sum = 0.0;
    std::size_t count = 0;
    for (std::size_t i = ch; i < samples.size(); i += channels) {
        const auto v = static_cast<double>(samples[i]);
        sum += v * v;
        ++count;
    }
    return count > 0U ? static_cast<float>(std::sqrt(sum / static_cast<double>(count))) : 0.0F;
}

#endif // __APPLE__

bool file_contains_bytes(const std::string& path, std::string_view needle) {
    std::ifstream in(path, std::ios::binary);
    if (!check(in.good(), "open file for byte scan")) {
        return false;
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return bytes.find(needle) != std::string::npos;
}

// Verifies that the wav71 swap (ch4↔ch6, ch5↔ch7) is actually applied.
//
// Strategy: encode a WAVE_7_1 WAV with ch4=0.8, ch5=0.7, ch6=0.2, ch7=0.3
// amplitude (all other channels 0.5).  After the swap the APAC encoder sees
// ch4=0.2, ch5=0.3, ch6=0.8, ch7=0.7 in AU_7_1 order.  Decoding back to PCM
// must therefore give rms(ch4) < rms(ch6) and rms(ch5) < rms(ch7).
// If the swap were omitted the inequalities would be reversed.
bool verify_apac_wav71_swap() {
    constexpr uint32_t k_ch = 8U;
    const std::string wav = mr_test::temp_prefix() + "mr_apac_wav71_src.wav";
    const std::string m4a = mr_test::temp_prefix() + "mr_apac_wav71.m4a";
    FileGuard gw(wav);
    FileGuard gm(m4a);

    // WAVE_7_1 physical order: L R C LFE Rls(0.8) Rrs(0.7) Ls(0.2) Rs(0.3)
    const std::vector<float> amps = {0.5F, 0.5F, 0.5F, 0.5F, 0.8F, 0.7F, 0.2F, 0.3F};
    if (!write_test_wav_with_amps(wav, k_ch, amps)) {
        return false;
    }

    CapturingProgressSink progress;
    auto res =
        mradm::audio::convert_to_apac(wav, m4a, "wav71", 0U, true, mradm::audio::ApacContainer::mpeg4, {}, &progress);
    if (!check(res.has_value(), "convert_to_apac(wav71) failed")) {
        return false;
    }
    if (!verify_apac_progress(progress, 48000U)) {
        return false;
    }
    if (!check(std::filesystem::exists(m4a), "wav71 .m4a not created")) {
        return false;
    }
    if (!check(std::filesystem::file_size(m4a) > 10000U, "wav71 .m4a suspiciously small")) {
        return false;
    }

#ifdef __APPLE__
    // Round-trip decode: verify swap was applied.
    std::vector<float> decoded;
    if (!check(decode_apac_to_pcm(m4a, k_ch, decoded), "APAC decode failed")) {
        return false;
    }
    const float rms4 = channel_rms(decoded, 4, k_ch);
    const float rms5 = channel_rms(decoded, 5, k_ch);
    const float rms6 = channel_rms(decoded, 6, k_ch);
    const float rms7 = channel_rms(decoded, 7, k_ch);
    // After swap: AU_7_1 ch4(Ls)=0.2 < ch6(Rls)=0.8 and ch5(Rs)=0.3 < ch7(Rrs)=0.7
    if (!check(rms4 < rms6, "wav71 swap not applied: decoded ch4 should be weaker than ch6")) {
        return false;
    }
    if (!check(rms5 < rms7, "wav71 swap not applied: decoded ch5 should be weaker than ch7")) {
        return false;
    }
#endif

    return true;
}

bool verify_apac_atmos514() {
    const std::string wav = mr_test::temp_prefix() + "mr_apac_atmos514_src.wav";
    const std::string m4a = mr_test::temp_prefix() + "mr_apac_atmos514.m4a";
    FileGuard gw(wav);
    FileGuard gm(m4a);
    if (!write_test_wav(wav, 10, 48000, 48000)) {
        return false;
    }
    auto res = mradm::audio::convert_to_apac(wav, m4a, "5.1.4");
    if (!check(res.has_value(), "convert_to_apac(5.1.4) failed")) {
        return false;
    }
    if (!check(std::filesystem::exists(m4a), "atmos514 .m4a not created")) {
        return false;
    }
    return check(std::filesystem::file_size(m4a) > 10000U, "atmos514 .m4a suspiciously small");
}

bool verify_apac_atmos714() {
    const std::string wav = mr_test::temp_prefix() + "mr_apac_atmos714_src.wav";
    const std::string m4a = mr_test::temp_prefix() + "mr_apac_atmos714.m4a";
    FileGuard gw(wav);
    FileGuard gm(m4a);
    if (!write_test_wav(wav, 12, 48000, 48000)) {
        return false;
    }
    auto res = mradm::audio::convert_to_apac(wav, m4a, "7.1.4");
    if (!check(res.has_value(), "convert_to_apac(7.1.4) failed")) {
        return false;
    }
    if (!check(std::filesystem::exists(m4a), "atmos714 .m4a not created")) {
        return false;
    }
    return check(std::filesystem::file_size(m4a) > 10000U, "atmos714 .m4a suspiciously small");
}

bool verify_apac_atmos916() {
    const std::string wav = mr_test::temp_prefix() + "mr_apac_atmos916_src.wav";
    const std::string m4a = mr_test::temp_prefix() + "mr_apac_atmos916.m4a";
    FileGuard gw(wav);
    FileGuard gm(m4a);
    if (!write_test_wav(wav, 16, 48000, 48000)) {
        return false;
    }
    auto res = mradm::audio::convert_to_apac(wav, m4a, "9.1.6");
    if (!check(res.has_value(), "convert_to_apac(9.1.6) failed")) {
        return false;
    }
    if (!check(std::filesystem::exists(m4a), "atmos916 .m4a not created")) {
        return false;
    }
    return check(std::filesystem::file_size(m4a) > 10000U, "atmos916 .m4a suspiciously small");
}

bool verify_apac_222() {
    const std::string wav = mr_test::temp_prefix() + "mr_apac_222_src.wav";
    const std::string m4a = mr_test::temp_prefix() + "mr_apac_222.m4a";
    FileGuard gw(wav);
    FileGuard gm(m4a);
    if (!write_test_wav(wav, 24, 48000, 48000)) {
        return false;
    }
    auto res = mradm::audio::convert_to_apac(wav, m4a, "22.2");
    if (!check(res.has_value(), "convert_to_apac(22.2) failed")) {
        return false;
    }
    if (!check(std::filesystem::exists(m4a), "22.2 .m4a not created")) {
        return false;
    }
    return check(std::filesystem::file_size(m4a) > 10000U, "22.2 .m4a suspiciously small");
}

bool verify_apac_hoa3() {
    const std::string wav = mr_test::temp_prefix() + "mr_apac_hoa3_src.wav";
    const std::string m4a = mr_test::temp_prefix() + "mr_apac_hoa3.m4a";
    FileGuard gw(wav);
    FileGuard gm(m4a);

    if (!write_test_wav(wav, 16U, 48000U, 48000U)) {
        return false;
    }
    auto res = mradm::audio::convert_to_apac(wav, m4a, "hoa3");
    if (!check(res.has_value(), "convert_to_apac(hoa3) failed")) {
        return false;
    }
    if (!check(std::filesystem::exists(m4a), "HOA3 .m4a not created")) {
        return false;
    }
    if (!check(std::filesystem::file_size(m4a) > 10000U, "HOA3 .m4a suspiciously small")) {
        return false;
    }
#ifdef __APPLE__
    std::vector<float> decoded;
    return check(decode_apac_to_pcm(m4a, 16U, decoded), "HOA3 APAC decodes as 16ch");
#else
    return true;
#endif
}

bool verify_apac_binaural() {
    const std::string wav = mr_test::temp_prefix() + "mr_apac_binaural_src.wav";
    const std::string m4a = mr_test::temp_prefix() + "mr_apac_binaural.m4a";
    FileGuard gw(wav);
    FileGuard gm(m4a);
    if (!write_test_wav(wav, 2, 48000, 48000)) {
        return false;
    }
    mradm::audio::MetadataFields meta;
    meta.encoder = "MacinRender Test";
    meta.date_utc = "2026-05-23T03:42:00Z";
    meta.renderer = "binaural-hrtf";
    meta.output_layout = "binaural";
    auto res = mradm::audio::convert_to_apac(wav, m4a, "binaural");
    if (!check(res.has_value(), "convert_to_apac(binaural) failed")) {
        std::cerr << res.error().message << "\n";
        return false;
    }
    if (!check(std::filesystem::exists(m4a), "binaural .m4a not created")) {
        return false;
    }
    if (!check(std::filesystem::file_size(m4a) > 10000U, "binaural .m4a suspiciously small")) {
        return false;
    }
    auto meta_res = mradm::audio::write_file_metadata(m4a, meta);
    if (!check(meta_res.has_value(), "write_file_metadata(.m4a) failed")) {
        std::cerr << meta_res.error().message << "\n";
        return false;
    }
    if (!check(file_contains_bytes(m4a,
                                   "\xA9"
                                   "cmt"),
               "APAC metadata contains iTunes comment atom")) {
        return false;
    }
    if (!check(file_contains_bytes(m4a, "layout=binaural"), "APAC metadata comment preserves layout=binaural")) {
        return false;
    }
#ifdef __APPLE__
    std::vector<float> decoded;
    return check(decode_apac_to_pcm(m4a, 2U, decoded), "APAC still decodes after metadata insertion");
#else
    return true;
#endif
}

bool verify_apac_caf_container() {
    const std::string wav = mr_test::temp_prefix() + "mr_apac_caf_src.wav";
    const std::string caf = mr_test::temp_prefix() + "mr_apac_caf.caf";
    FileGuard gw(wav);
    FileGuard gc(caf);
    if (!write_test_wav(wav, 10, 48000, 48000)) {
        return false;
    }
    auto res = mradm::audio::convert_to_apac(wav, caf, "5.1.4", 0U, true, mradm::audio::ApacContainer::caf);
    if (!check(res.has_value(), "convert_to_apac(5.1.4 CAF) failed")) {
        if (!res.has_value()) {
            std::cerr << res.error().message << "\n";
        }
        return false;
    }
    if (!check(std::filesystem::exists(caf), "APAC CAF not created")) {
        return false;
    }
    if (!check(std::filesystem::file_size(caf) > 10000U, "APAC CAF suspiciously small")) {
        return false;
    }
    if (!check(file_contains_bytes(caf, "caff"), "APAC CAF contains CAF magic")) {
        return false;
    }
    if (!check(file_contains_bytes(caf, "apac"), "APAC CAF desc contains apac format id")) {
        return false;
    }
#ifdef __APPLE__
    std::vector<float> decoded;
    return check(decode_apac_to_pcm(caf, 10U, decoded), "APAC CAF decodes as 10ch");
#else
    return true;
#endif
}

// Drives the parent-side stall watchdog: the encoder runs in a forked mradm
// subprocess, and MRADM_APAC_TEST_FORCE_STALL makes that child spin forever in
// the flush phase (emulating the AudioToolbox spin-hang observed at pathological
// bitrates). With the flush budget shrunk via env, the watchdog must SIGKILL the
// child and surface a clear error in well under the real 15 s budget — never hang.
// Requires the subprocess path (helper set via MRADM_APAC_HELPER by the caller).
bool verify_apac_stall_watchdog() {
    const std::string wav = mr_test::temp_prefix() + "mr_apac_stall_src.wav";
    const std::string m4a = mr_test::temp_prefix() + "mr_apac_stall.m4a";
    FileGuard gw(wav);
    FileGuard gm(m4a);
    if (!write_test_wav(wav, 10, 48000, 48000)) { // 5.1.4, 1 s
        return false;
    }
    mr_test::set_env("MRADM_APAC_TEST_FORCE_STALL", "1");
    mr_test::set_env("MRADM_APAC_FLUSH_BUDGET_MS", "1500");
    const auto t0 = std::chrono::steady_clock::now();
    auto res = mradm::audio::convert_to_apac(wav, m4a, "5.1.4");
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    mr_test::unset_env("MRADM_APAC_TEST_FORCE_STALL");
    mr_test::unset_env("MRADM_APAC_FLUSH_BUDGET_MS");

    bool ok = check(!res.has_value(), "forced flush stall must surface as an error, not a hang");
    if (res.has_value()) {
        return false;
    }
    ok = check(res.error().code == mradm::ErrorCode::io_error, "stall error code should be io_error") && ok;
    ok = check(res.error().message.find("停滞") != std::string::npos, "stall error should advise lowering bitrate") &&
         ok;
    ok = check(elapsed < std::chrono::seconds{10}, "watchdog should fire within the shrunk budget, not hang") && ok;
    ok = check(!std::filesystem::exists(m4a), "wedged/partial output should be removed on stall") && ok;
    return ok;
}

bool verify_apac_wrong_layout_rejected() {
    auto res = mradm::audio::convert_to_apac(mr_test::temp_prefix() + "nope.wav", mr_test::temp_prefix() + "nope.m4a", "bogus-layout");
    return check(!res.has_value() && res.error().code == mradm::ErrorCode::unsupported,
                 "unsupported layout should be rejected");
}

bool verify_apac_wrong_samplerate_rejected() {
    const std::string wav = mr_test::temp_prefix() + "mr_apac_44k_src.wav";
    const std::string m4a = mr_test::temp_prefix() + "mr_apac_44k.m4a";
    FileGuard gw(wav);
    FileGuard gm(m4a);
    if (!write_test_wav(wav, 2, 44100, 44100)) {
        return false;
    }
    auto res = mradm::audio::convert_to_apac(wav, m4a, "0+2+0");
    return check(!res.has_value() && res.error().code == mradm::ErrorCode::invalid_argument,
                 "44100 Hz input should be rejected for APAC");
}

bool verify_apac_wrong_channelcount_rejected() {
    const std::string wav = mr_test::temp_prefix() + "mr_apac_2ch_wav71.wav";
    const std::string m4a = mr_test::temp_prefix() + "mr_apac_2ch_wav71.m4a";
    FileGuard gw(wav);
    FileGuard gm(m4a);
    if (!write_test_wav(wav, 2, 48000, 480)) {
        return false;
    }
    auto res = mradm::audio::convert_to_apac(wav, m4a, "wav71");
    return check(!res.has_value() && res.error().code == mradm::ErrorCode::invalid_argument,
                 "2ch WAV with wav71 layout (requires 8ch) should be rejected");
}

} // namespace

int main() {
#ifndef __APPLE__
    std::cout << "APAC smoke tests skipped (not macOS)\n";
    return EXIT_SUCCESS;
#elif !defined(MRADM_EXE_PATH)
    // APAC now always encodes via a forked mradm subprocess (stall watchdog).
    // Without the CLI there is no helper to fork, so the encoder is intentionally
    // unavailable and there is nothing to exercise here.
    std::cout << "APAC smoke tests skipped (requires the bundled mradm helper / CLI build)\n";
    return EXIT_SUCCESS;
#else
    // Pin the subprocess helper to the freshly built mradm so the tests are
    // hermetic (and the force-stall hook reachable) regardless of discovery.
    mr_test::set_env("MRADM_APAC_HELPER", MRADM_EXE_PATH);
    bool ok = true;
    ok &= verify_apac_wav71_swap();
    ok &= verify_apac_atmos514();
    ok &= verify_apac_atmos714();
    ok &= verify_apac_atmos916();
    ok &= verify_apac_222();
    ok &= verify_apac_hoa3();
    ok &= verify_apac_binaural();
    ok &= verify_apac_caf_container();
    ok &= verify_apac_wrong_layout_rejected();
    ok &= verify_apac_wrong_samplerate_rejected();
    ok &= verify_apac_wrong_channelcount_rejected();
    ok &= verify_apac_stall_watchdog();
    if (!ok) {
        return EXIT_FAILURE;
    }
    std::cout << "APAC smoke tests passed\n";
    return EXIT_SUCCESS;
#endif
}
