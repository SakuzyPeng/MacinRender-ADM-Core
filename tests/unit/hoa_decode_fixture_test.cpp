#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <adm/adm.hpp>
#include <adm/utilities/id_assignment.hpp>
#include <adm/write.hpp>
#include <bw64/bw64.hpp>

#include "adm/audio_io.h"
#include "adm/options.h"
#include "adm/render.h"

namespace {

constexpr uint32_t k_sample_rate = 48000U;
constexpr uint32_t k_frames = 2048U;
constexpr float k_amplitude = 0.5F;
constexpr std::size_t k_hoa1_channels = 4U;

// ACN order (SN3D): (order, degree) for each of the 4 HOA1 channels.
// Index 0 = W (0,0), 1 = Y (1,-1), 2 = Z (1,0), 3 = X (1,+1).
struct HoaChannelSpec {
    int order;
    int degree;
};
constexpr std::array<HoaChannelSpec, k_hoa1_channels> k_hoa1_spec = {{{0, 0}, {1, -1}, {1, 0}, {1, 1}}};

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

bool check(bool condition, const char* msg) {
    if (!condition) {
        std::cerr << "FAIL: " << msg << "\n";
    }
    return condition;
}

// Build a first-order Ambisonics (HOA1) ADM document with 4 channels.
// Returns (document, uid_strings[4]) where uid_strings[i] is the ATU ID for
// HOA channel i (ACN order: 0=W, 1=Y, 2=Z, 3=X).
std::pair<std::shared_ptr<adm::Document>, std::vector<std::string>> make_hoa1_doc() {
    auto doc = adm::Document::create();

    auto pf = adm::AudioPackFormatHoa::create(adm::AudioPackFormatName{"HOA1PF"});
    doc->add(pf);

    auto obj = adm::AudioObject::create(adm::AudioObjectName{"HOA1Obj"});
    doc->add(obj);

    std::size_t idx = 0;
    for (const auto& spec : k_hoa1_spec) {
        const std::string suffix = std::to_string(idx);
        auto cf =
            adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"HOA1CF_" + suffix}, adm::TypeDefinition::HOA);
        adm::AudioBlockFormatHoa block{adm::Order{spec.order}, adm::Degree{spec.degree}};
        cf->add(block);
        doc->add(cf);
        pf->addReference(cf);

        auto sf =
            adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"HOA1SF_" + suffix}, adm::FormatDefinition::PCM);
        sf->setReference(cf);
        doc->add(sf);

        auto tf =
            adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"HOA1TF_" + suffix}, adm::FormatDefinition::PCM);
        tf->setReference(sf);
        sf->addReference(tf);
        doc->add(tf);

        auto uid = adm::AudioTrackUid::create();
        uid->setReference(tf);
        uid->setReference(pf);
        doc->add(uid);

        obj->addReference(uid);
        ++idx;
    }

    auto content = adm::AudioContent::create(adm::AudioContentName{"HOA1Content"});
    content->addReference(obj);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"HOA1Prog"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);

    // Collect UID strings in AudioObject reference order after ID assignment.
    const auto uid_refs = obj->getReferences<adm::AudioTrackUid>();
    std::vector<std::string> uid_strs;
    uid_strs.reserve(uid_refs.size());
    std::ranges::transform(uid_refs, std::back_inserter(uid_strs), [](const std::shared_ptr<adm::AudioTrackUid>& u) {
        return adm::formatId(u->get<adm::AudioTrackUidId>());
    });

    return {doc, uid_strs};
}

// Write a 4-channel HOA1 BW64 file.
// signal_ch: which channel carries k_amplitude signal; all others are silent.
std::filesystem::path write_hoa1_fixture(const std::shared_ptr<adm::Document>& doc,
                                         const std::vector<std::string>& uid_strs,
                                         std::size_t signal_ch,
                                         const std::string& suffix) {
    auto path = std::filesystem::temp_directory_path() / ("mr_hoa_dec_" + suffix + ".wav");

    std::vector<bw64::AudioId> audio_ids;
    audio_ids.reserve(uid_strs.size());
    for (std::size_t i = 0; i < uid_strs.size(); ++i) {
        audio_ids.emplace_back(static_cast<uint16_t>(i + 1U), uid_strs[i], "", "");
    }
    auto chna = std::make_shared<bw64::ChnaChunk>(audio_ids);

    std::ostringstream xml_buf;
    adm::writeXml(xml_buf, doc);
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());

    auto writer =
        bw64::writeFile(path.string(), static_cast<uint16_t>(k_hoa1_channels), k_sample_rate, 24U, chna, axml);

    // Interleaved frame: [ch0_s0, ch1_s0, ..., chN_s0, ch0_s1, ...]
    std::vector<float> buf(k_frames * k_hoa1_channels, 0.0F);
    for (std::size_t f = 0; f < k_frames; ++f) {
        buf[(f * k_hoa1_channels) + signal_ch] = k_amplitude;
    }
    writer->write(buf.data(), k_frames);
    return path;
}

// Read per-channel RMS from an output file with num_ch channels.
std::vector<double> read_rms(const std::filesystem::path& path, std::size_t num_ch) {
    std::vector<double> rms(num_ch, 0.0);
    auto res = mradm::audio::FloatWavReader::open(path.string());
    if (!res) {
        return rms;
    }
    auto& reader = *res;
    const auto n_frames = static_cast<std::size_t>(reader.frame_count());
    std::vector<float> samples(n_frames * num_ch, 0.0F);
    reader.read(samples.data(), reader.frame_count());

    for (std::size_t f = 0; f < n_frames; ++f) {
        for (std::size_t c = 0; c < num_ch; ++c) {
            const auto s = static_cast<double>(samples[(f * num_ch) + c]);
            rms[c] += s * s;
        }
    }
    std::ranges::transform(
        rms, rms.begin(), [n_frames](double r) { return std::sqrt(r / static_cast<double>(n_frames)); });
    return rms;
}

// Read per-channel RMS for a single channel over [start_frame, end_frame).
double rms_channel_slice(const std::filesystem::path& path,
                         std::size_t num_ch,
                         std::size_t ch_idx,
                         std::size_t start_frame,
                         std::size_t end_frame) {
    auto res = mradm::audio::FloatWavReader::open(path.string());
    if (!res) {
        return 0.0;
    }
    auto& reader = *res;
    const auto n_total = static_cast<std::size_t>(reader.frame_count());
    const std::size_t end = std::min(end_frame, n_total);
    std::vector<float> samples(n_total * num_ch, 0.0F);
    reader.read(samples.data(), reader.frame_count());
    double acc = 0.0;
    for (std::size_t f = start_frame; f < end; ++f) {
        const auto s = static_cast<double>(samples[(f * num_ch) + ch_idx]);
        acc += s * s;
    }
    return (end > start_frame) ? std::sqrt(acc / static_cast<double>(end - start_frame)) : 0.0;
}

// Add a CF→SF→TF→UID chain to doc/pf/obj; returns the UID.
// blocks is the list of AudioBlockFormatHoa to add to the CF.
std::shared_ptr<adm::AudioTrackUid> add_hoa1_channel_chain(adm::Document& doc,
                                                           const std::shared_ptr<adm::AudioPackFormatHoa>& pf,
                                                           const std::shared_ptr<adm::AudioObject>& obj,
                                                           const std::string& sfx,
                                                           const std::vector<adm::AudioBlockFormatHoa>& blocks) {
    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"HOA1CF_" + sfx}, adm::TypeDefinition::HOA);
    for (const auto& blk : blocks) {
        cf->add(blk);
    }
    doc.add(cf);
    pf->addReference(cf);
    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"HOA1SF_" + sfx}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc.add(sf);
    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"HOA1TF_" + sfx}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc.add(tf);
    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc.add(uid);
    obj->addReference(uid);
    return uid;
}

// Build a HOA1 doc where each channel has two AudioBlockFormatHoa blocks:
//   block 0: rtime=0, duration=split_ns, gain=1.0
//   block 1: rtime=split_ns, no explicit duration, gain=0.0
std::pair<std::shared_ptr<adm::Document>, std::vector<std::string>> make_hoa1_doc_two_blocks(int64_t split_ns) {
    auto doc = adm::Document::create();
    auto pf = adm::AudioPackFormatHoa::create(adm::AudioPackFormatName{"HOA1PF"});
    doc->add(pf);
    auto obj = adm::AudioObject::create(adm::AudioObjectName{"HOA1Obj"});
    doc->add(obj);

    std::size_t idx = 0;
    for (const auto& spec : k_hoa1_spec) {
        const adm::Time split{std::chrono::nanoseconds{split_ns}};
        adm::AudioBlockFormatHoa blk1{adm::Order{spec.order}, adm::Degree{spec.degree}, adm::Duration{split}};
        adm::AudioBlockFormatHoa blk2{
            adm::Order{spec.order}, adm::Degree{spec.degree}, adm::Rtime{split}, adm::Gain{0.0}};
        add_hoa1_channel_chain(*doc, pf, obj, std::to_string(idx++), {blk1, blk2});
    }
    auto content = adm::AudioContent::create(adm::AudioContentName{"HOA1Content"});
    content->addReference(obj);
    doc->add(content);
    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"HOA1Prog"});
    prog->addReference(content);
    doc->add(prog);
    adm::reassignIds(doc);

    const auto uid_refs = obj->getReferences<adm::AudioTrackUid>();
    std::vector<std::string> uid_strs;
    uid_strs.reserve(uid_refs.size());
    std::ranges::transform(uid_refs, std::back_inserter(uid_strs), [](const std::shared_ptr<adm::AudioTrackUid>& u) {
        return adm::formatId(u->get<adm::AudioTrackUidId>());
    });
    return {doc, uid_strs};
}

// Build a HOA1 doc with 4 proper channels plus one phantom UID (no AudioTrackFormat
// → channel_format_from_uid returns nullptr → importer must skip it).
// Returns {doc, proper_uid_strs[4], phantom_uid_str}.
std::tuple<std::shared_ptr<adm::Document>, std::vector<std::string>, std::string> make_hoa1_doc_with_phantom() {
    auto doc = adm::Document::create();
    auto pf = adm::AudioPackFormatHoa::create(adm::AudioPackFormatName{"HOA1PF"});
    doc->add(pf);
    auto obj = adm::AudioObject::create(adm::AudioObjectName{"HOA1Obj"});
    doc->add(obj);

    std::size_t idx = 0;
    for (const auto& spec : k_hoa1_spec) {
        adm::AudioBlockFormatHoa blk{adm::Order{spec.order}, adm::Degree{spec.degree}};
        add_hoa1_channel_chain(*doc, pf, obj, std::to_string(idx++), {blk});
    }

    // Phantom UID: references the HOA pack format but has no AudioTrackFormat chain.
    // channel_format_from_uid() returns nullptr → no HOA blocks → channel is skipped.
    auto phantom_uid = adm::AudioTrackUid::create();
    phantom_uid->setReference(pf);
    doc->add(phantom_uid);
    obj->addReference(phantom_uid);

    auto content = adm::AudioContent::create(adm::AudioContentName{"HOA1Content"});
    content->addReference(obj);
    doc->add(content);
    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"HOA1Prog"});
    prog->addReference(content);
    doc->add(prog);
    adm::reassignIds(doc);

    const auto uid_refs = obj->getReferences<adm::AudioTrackUid>();
    const std::string phantom_str = adm::formatId(phantom_uid->get<adm::AudioTrackUidId>());
    std::vector<std::string> proper_uids;
    proper_uids.reserve(k_hoa1_channels);
    for (const auto& u : uid_refs) {
        const auto s = adm::formatId(u->get<adm::AudioTrackUidId>());
        if (s != phantom_str) {
            proper_uids.push_back(s);
        }
    }
    return {doc, proper_uids, phantom_str};
}

// W channel (order=0, degree=0) is omnidirectional. Decoded to 0+5+0 (5.1),
// all non-LFE speakers should receive approximately equal energy.
bool verify_hoa1_w_decodes_to_all_speakers() {
    auto [doc, uid_strs] = make_hoa1_doc();
    const auto in_path = write_hoa1_fixture(doc, uid_strs, 0U, "w_in");
    FileGuard in_guard{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_hoa_dec_w_out.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.renderer = mradm::RendererSelection::ear;
    req.options.output_layout = "0+5+0";
    req.options.peak_limit = false;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: HOA1 W decode render failed: " << res.error.message << "\n";
        return false;
    }

    auto hdr_res = mradm::audio::FloatWavReader::open(out_path.string());
    if (!hdr_res) {
        std::cerr << "FAIL: cannot open HOA1 W decode output\n";
        return false;
    }
    bool ok = true;
    ok &= check(hdr_res->channels() == 6U, "HOA1 W decode: output is 6-channel (0+5+0)");
    if (!ok) {
        return false;
    }

    const auto rms = read_rms(out_path, 6);
    // 0+5+0: L R C LFE Ls Rs (indices 0–5); LFE = ch3.
    // W is omnidirectional — all non-LFE speakers should have energy.
    ok &= check(rms[0] > 0.01, "HOA1 W decode: L (ch0) has energy");
    ok &= check(rms[1] > 0.01, "HOA1 W decode: R (ch1) has energy");
    ok &= check(rms[2] > 0.01, "HOA1 W decode: C (ch2) has energy");
    ok &= check(rms[3] < 1e-5, "HOA1 W decode: LFE (ch3) is silent");
    ok &= check(rms[4] > 0.01, "HOA1 W decode: Ls (ch4) has energy");
    ok &= check(rms[5] > 0.01, "HOA1 W decode: Rs (ch5) has energy");
    // L/R should be approximately equal (symmetric decoder for symmetric layout).
    ok &= check(std::abs(rms[0] - rms[1]) < rms[0] * 0.1, "HOA1 W decode: L ≈ R (symmetric)");
    return ok;
}

// Muted AudioObject: HOA decode must produce silence, not fail.
bool verify_hoa1_mute_produces_silence() {
    auto [doc, uid_strs] = make_hoa1_doc();
    for (const auto& ao : doc->getElements<adm::AudioObject>()) {
        ao->set(adm::Mute{true});
    }
    const auto in_path = write_hoa1_fixture(doc, uid_strs, 0U, "mute_in");
    FileGuard in_guard{in_path};

    const auto out_path = std::filesystem::temp_directory_path() / "mr_hoa_dec_mute_out.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.renderer = mradm::RendererSelection::ear;
    req.options.output_layout = "0+5+0";
    req.options.peak_limit = false;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: muted HOA1 must write silence, not fail: " << res.error.message << "\n";
        return false;
    }

    const auto rms = read_rms(out_path, 6);
    bool ok = true;
    for (std::size_t ch = 0; ch < 6; ++ch) {
        ok &= check(rms[ch] < 1e-6, "HOA1 muted: all channels silent");
    }
    return ok;
}

// AudioObject.gain=0.5 should halve the output amplitude relative to gain=1.
bool verify_hoa1_obj_gain_scales_output() {
    auto [doc1, uid_strs1] = make_hoa1_doc();
    auto [doc2, uid_strs2] = make_hoa1_doc();
    for (const auto& ao : doc2->getElements<adm::AudioObject>()) {
        ao->set(adm::Gain{0.5});
    }

    const auto in1 = write_hoa1_fixture(doc1, uid_strs1, 0U, "gain1_in");
    FileGuard g1{in1};
    const auto in2 = write_hoa1_fixture(doc2, uid_strs2, 0U, "gain2_in");
    FileGuard g2{in2};

    const auto out1 = std::filesystem::temp_directory_path() / "mr_hoa_dec_gain1_out.wav";
    FileGuard o1{out1};
    const auto out2 = std::filesystem::temp_directory_path() / "mr_hoa_dec_gain2_out.wav";
    FileGuard o2{out2};

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;

    auto render = [&](const auto& in, const auto& out) {
        mradm::RenderRequest req;
        req.input_path = in;
        req.output_path = out;
        req.options.renderer = mradm::RendererSelection::ear;
        req.options.output_layout = "0+5+0";
        req.options.peak_limit = false;
        return service.render(req, progress, logs);
    };

    if (const auto r = render(in1, out1); !r.success()) {
        std::cerr << "FAIL: HOA1 gain=1 render failed: " << r.error.message << "\n";
        return false;
    }
    if (const auto r = render(in2, out2); !r.success()) {
        std::cerr << "FAIL: HOA1 gain=0.5 render failed: " << r.error.message << "\n";
        return false;
    }

    const auto rms1 = read_rms(out1, 6);
    const auto rms2 = read_rms(out2, 6);

    // gain=0.5 → amplitude halved → RMS halved; check L channel (ch0).
    const double ratio = (rms1[0] > 1e-9) ? (rms2[0] / rms1[0]) : 0.0;
    return check(ratio > 0.45 && ratio < 0.55, "HOA1 obj_gain=0.5: output RMS ≈ half of gain=1");
}

// Multi-block HOA: block 1 has gain=1.0 (frames 0..k_half-1),
// block 2 has gain=0.0 (frames k_half..k_frames-1).
// First half of output must have energy; second half must be silent.
bool verify_hoa1_multiblock_time_gating() {
    // 21333334 ns → exactly 1024 samples at 48 kHz (verified via time_to_samples arithmetic).
    constexpr int64_t k_split_ns = 21333334LL;
    constexpr std::size_t k_half = k_frames / 2U;

    auto [doc, uid_strs] = make_hoa1_doc_two_blocks(k_split_ns);
    const auto in_path = write_hoa1_fixture(doc, uid_strs, 0U, "mb_in");
    FileGuard in_guard{in_path};
    const auto out_path = std::filesystem::temp_directory_path() / "mr_hoa_dec_mb_out.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.renderer = mradm::RendererSelection::ear;
    req.options.output_layout = "0+5+0";
    req.options.peak_limit = false;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: HOA1 multi-block render failed: " << res.error.message << "\n";
        return false;
    }

    // 0+5+0 L channel (ch0).
    // The EAR renderer's 255-sample direct-bus compensation delay bleeds the
    // tail of block1's signal into the first ~255 frames of the second block.
    // Check energy in first half, then silence strictly beyond the delay tail.
    constexpr std::size_t k_comp_delay = 256U; // 255 + 1 frame margin
    bool ok = true;
    ok &= check(rms_channel_slice(out_path, 6U, 0U, 0U, k_half) > 0.01,
                "HOA1 multi-block: first half (block gain=1) has energy");
    ok &= check(rms_channel_slice(out_path, 6U, 0U, k_half + k_comp_delay, k_frames) < 1e-5,
                "HOA1 multi-block: second half beyond delay tail (block gain=0) is silent");
    return ok;
}

// Phantom UID: a UID with no AudioTrackFormat (broken chain) must be skipped by the
// importer.  Signal is placed only on that phantom BW64 track; all output must be silent.
bool verify_hoa1_no_phantom_w_from_broken_uid() {
    auto [doc, proper_uids, phantom_str] = make_hoa1_doc_with_phantom();

    // 5 UIDs total (4 proper + 1 phantom); phantom is the last BW64 track (ch4).
    std::vector<std::string> all_uid_strs = proper_uids;
    all_uid_strs.push_back(phantom_str);
    constexpr std::size_t k_total_ch = k_hoa1_channels + 1U;

    const auto in_path = std::filesystem::temp_directory_path() / "mr_hoa_dec_phant_in.wav";
    FileGuard in_guard{in_path};
    {
        std::vector<bw64::AudioId> audio_ids;
        audio_ids.reserve(k_total_ch);
        for (std::size_t i = 0; i < k_total_ch; ++i) {
            audio_ids.emplace_back(static_cast<uint16_t>(i + 1U), all_uid_strs[i], "", "");
        }
        auto chna = std::make_shared<bw64::ChnaChunk>(audio_ids);
        std::ostringstream xml_buf;
        adm::writeXml(xml_buf, doc);
        auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());
        auto writer =
            bw64::writeFile(in_path.string(), static_cast<uint16_t>(k_total_ch), k_sample_rate, 24U, chna, axml);
        std::vector<float> buf(k_frames * k_total_ch, 0.0F);
        for (std::size_t f = 0; f < k_frames; ++f) {
            buf[(f * k_total_ch) + (k_total_ch - 1U)] = k_amplitude; // phantom track only
        }
        writer->write(buf.data(), k_frames);
    }

    const auto out_path = std::filesystem::temp_directory_path() / "mr_hoa_dec_phant_out.wav";
    FileGuard out_guard{out_path};

    mradm::RenderRequest req;
    req.input_path = in_path;
    req.output_path = out_path;
    req.options.renderer = mradm::RendererSelection::ear;
    req.options.output_layout = "0+5+0";
    req.options.peak_limit = false;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const auto res = service.render(req, progress, logs);
    if (!res.success()) {
        std::cerr << "FAIL: phantom UID render failed: " << res.error.message << "\n";
        return false;
    }

    const auto rms = read_rms(out_path, 6);
    bool ok = true;
    for (std::size_t ch = 0; ch < 6U; ++ch) {
        ok &= check(rms[ch] < 1e-5, "HOA1 phantom UID: skipped — no phantom W contribution");
    }
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= verify_hoa1_w_decodes_to_all_speakers();
    ok &= verify_hoa1_mute_produces_silence();
    ok &= verify_hoa1_obj_gain_scales_output();
    ok &= verify_hoa1_multiblock_time_gating();
    ok &= verify_hoa1_no_phantom_w_from_broken_uid();

    if (ok) {
        std::cout << "hoa decode fixture test passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
