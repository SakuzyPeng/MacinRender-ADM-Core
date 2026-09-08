// Deterministic ADM BWF fixture generator for the cross-platform numerical baseline.
//
// The baseline compares final float32 PCM across macOS / Linux / Windows. That is only
// meaningful if the *inputs* are identical too, so this generator is bit-exact by construction
// (roadmap §6.1: "不能让各平台分别用未受控的 sin() 生成输入"):
//
//   - The sample values come from a 32-bit LCG. The integer is mapped to float by subtracting
//     2^23 and dividing by 2^23. Both steps are exact in binary32 (|v| <= 2^23 converts
//     exactly, and the divisor is a power of two), so no libm and no rounding choice is
//     involved. libbw64 subsequently applies its fixed 24-bit quantisation; the generated
//     container bytes are compared across platforms too.
//   - Positions, extents and gains are exact decimals that round-trip through the ADM XML text.
//
// CI compares generated input bytes before interpreting output differences; this
// generator is designed so that check passes, not assumed to.
//
// Kinds:
//   objects-point    1 Objects channel, no extent  — clean gain path, no decorrelator
//   objects-extent   1 Objects channel with width/height/diffuse — drives the decorrelator and,
//                    with --binaural-spread-mode saf-spreader, the SAF OM spreader
//   objects-extent-multi  3 Objects tracks — enables multiple OLA sources / spreader groups
//   directspeakers   5.1 bed — DirectSpeakers gain calculation plus LFE routing
//   hoa              HOA1 (4 channels) — HOA-typed input path
//   objects-cartesian  1 Objects channel with CartesianPosition + extent — the only fixture that
//                    leaves block.position.cartesian == true, which is what distance_from_position
//                    (hoa/vbap/binaural) and mdap_spread_degrees branch on. Without it those
//                    length computations are dead code in the matrix.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <adm/adm.hpp>
#include <adm/utilities/id_assignment.hpp>
#include <adm/write.hpp>
#include <bw64/bw64.hpp>

namespace {

constexpr uint32_t k_sample_rate = 48000;
constexpr uint32_t k_frames = 48000; // 1 s — keep CI artifacts small
constexpr uint16_t k_bit_depth = 24;

// Numerical Recipes LCG. Chosen for being trivially reproducible, not for statistical quality:
// the signal only needs to be broadband and identical everywhere.
[[nodiscard]] uint32_t lcg_next(uint32_t& state) {
    state = (state * 1664525U) + 1013904223U;
    return state;
}

// Exact in binary32: (r >> 8) is in [0, 2^24), so v is in [-2^23, 2^23) and converts exactly;
// dividing by 2^23 only adjusts the exponent.
[[nodiscard]] float exact_sample(uint32_t r) {
    const auto v = static_cast<int32_t>(r >> 8U) - 8388608;
    return static_cast<float>(v) / 8388608.0F;
}

[[nodiscard]] std::vector<float> make_signal(uint32_t channels, uint32_t frames, uint32_t seed) {
    std::vector<float> out(static_cast<std::size_t>(frames) * channels);
    uint32_t state = seed;
    std::ranges::generate(out, [&state] { return exact_sample(lcg_next(state)); });
    return out;
}

struct BuiltDoc {
    std::shared_ptr<adm::Document> doc;
    std::vector<std::string> uids; // one per audio channel, in track order
};

// Wire one channel format into the pack/stream/track/uid chain and return the uid.
// Templated on the pack type: HOA packs are AudioPackFormatHoa, and libadm resolves
// setReference() on the concrete type, so passing the base pointer would be wrong.
template <typename PackPtr>
std::shared_ptr<adm::AudioTrackUid> wire_channel(const std::shared_ptr<adm::Document>& doc,
                                                 const std::shared_ptr<adm::AudioChannelFormat>& cf,
                                                 const PackPtr& pf,
                                                 const std::string& suffix) {
    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"SF_" + suffix}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"TF_" + suffix}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);
    return uid;
}

void finish_doc(const std::shared_ptr<adm::Document>& doc,
                const std::vector<std::shared_ptr<adm::AudioTrackUid>>& uids,
                const std::string& name) {
    auto obj = adm::AudioObject::create(adm::AudioObjectName{name + "Object"});
    for (const auto& uid : uids) {
        obj->addReference(uid);
    }
    doc->add(obj);

    auto content = adm::AudioContent::create(adm::AudioContentName{name + "Content"});
    content->addReference(obj);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{name + "Programme"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);
}

[[nodiscard]] BuiltDoc build_objects(bool with_extent, int track_count = 1) {
    auto doc = adm::Document::create();
    std::vector<std::shared_ptr<adm::AudioTrackUid>> uids;
    for (int i = 0; i < track_count; ++i) {
        const std::string suffix = track_count == 1 ? "" : std::to_string(i);
        auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"ObjCF" + suffix},
                                                  adm::TypeDefinition::OBJECTS);
        adm::AudioBlockFormatObjects block{
            adm::SphericalPosition{adm::Azimuth{30.0F - (static_cast<float>(i) * 60.0F)}, adm::Elevation{0.0F}}};
        if (with_extent) {
            // Width exceeds the spreader gate; diffuse keeps the OLA bus active as well.
            block.set(adm::Width{30.0F});
            block.set(adm::Height{10.0F});
            block.set(adm::Diffuse{0.5F});
        }
        cf->add(block);
        doc->add(cf);
        auto pf =
            adm::AudioPackFormat::create(adm::AudioPackFormatName{"ObjPF" + suffix}, adm::TypeDefinition::OBJECTS);
        pf->addReference(cf);
        doc->add(pf);
        uids.push_back(wire_channel(doc, cf, pf, "Obj" + suffix));
    }
    const std::string name = with_extent ? "ObjectsExtent" : "ObjectsPoint";
    finish_doc(doc, uids, name + (track_count == 1 ? "" : "Multi"));
    std::vector<std::string> uid_strs;
    uid_strs.reserve(uids.size());
    std::ranges::transform(uids, std::back_inserter(uid_strs), [](const auto& uid) {
        return adm::formatId(uid->template get<adm::AudioTrackUidId>());
    });
    return {doc, uid_strs};
}

// Cartesian counterpart of build_objects(true). Coordinates are chosen so the vector length is
// exactly representable: 0.25² + 0.75² + 0.375² = 0.765625 = 0.875². A correctly rounded length
// therefore returns exactly 0x3F600000; glibc's three-argument std::hypot returns 0x3F600001.
// The case is a decidable correctness probe, not an arbitrary tie-break between implementations.
[[nodiscard]] BuiltDoc build_objects_cartesian() {
    auto doc = adm::Document::create();
    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"ObjCartCF"}, adm::TypeDefinition::OBJECTS);
    adm::AudioBlockFormatObjects block{adm::CartesianPosition{adm::X{0.25F}, adm::Y{0.75F}, adm::Z{0.375F}}};
    block.set(adm::Cartesian{true});
    // Extent is required at all: distance_from_position is reached from the extent path, so a
    // point source would leave it dead even with cartesian coordinates.
    //
    // The values are normalised (0..1), which is what BS.2076 specifies for cartesian objects and
    // what mdap_spread_degrees assumes when it scales width by 60. The polar objects-extent
    // fixture carries degrees (Width 30), so there the same expression yields 1800 and
    // std::min(180, ...) saturates — the spread numerics are inert in that case. Keeping this
    // fixture normalised is what makes the VBAP spread path measurable at all.
    block.set(adm::Width{0.5F});
    block.set(adm::Height{0.25F});
    block.set(adm::Depth{0.125F});
    block.set(adm::Diffuse{0.5F});
    cf->add(block);
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"ObjCartPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    std::vector<std::shared_ptr<adm::AudioTrackUid>> uids{wire_channel(doc, cf, pf, "ObjCart")};
    finish_doc(doc, uids, "ObjectsCartesian");

    std::vector<std::string> uid_strs;
    uid_strs.reserve(uids.size());
    std::ranges::transform(uids, std::back_inserter(uid_strs), [](const auto& uid) {
        return adm::formatId(uid->template get<adm::AudioTrackUidId>());
    });
    return {doc, uid_strs};
}

[[nodiscard]] BuiltDoc build_direct_speakers() {
    struct Speaker {
        const char* label;
        float azimuth;
        float elevation;
    };
    // BS.2051 System B (0+5+0). Exact decimals so the XML round-trips bit-identically.
    constexpr std::array<Speaker, 6> k_speakers{{{"M+030", 30.0F, 0.0F},
                                                 {"M-030", -30.0F, 0.0F},
                                                 {"M+000", 0.0F, 0.0F},
                                                 {"LFE1", 0.0F, -30.0F},
                                                 {"M+110", 110.0F, 0.0F},
                                                 {"M-110", -110.0F, 0.0F}}};

    auto doc = adm::Document::create();
    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"DsPF"}, adm::TypeDefinition::DIRECT_SPEAKERS);
    doc->add(pf);

    std::vector<std::shared_ptr<adm::AudioTrackUid>> uids;
    std::vector<std::string> uid_strs;
    for (std::size_t i = 0; i < k_speakers.size(); ++i) {
        const auto& sp = k_speakers.at(i);
        const std::string suffix = "Ds" + std::to_string(i);
        auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"CF_" + suffix},
                                                  adm::TypeDefinition::DIRECT_SPEAKERS);
        adm::AudioBlockFormatDirectSpeakers block{
            adm::SphericalSpeakerPosition{adm::Azimuth{sp.azimuth}, adm::Elevation{sp.elevation}, adm::Distance{1.0F}}};
        block.add(adm::SpeakerLabel{sp.label});
        cf->add(block);
        doc->add(cf);
        pf->addReference(cf);
        uids.push_back(wire_channel(doc, cf, pf, suffix));
    }

    finish_doc(doc, uids, "DirectSpeakers");
    uid_strs.reserve(uids.size());
    std::ranges::transform(uids, std::back_inserter(uid_strs), [](const auto& uid) {
        return adm::formatId(uid->template get<adm::AudioTrackUidId>());
    });
    return {doc, uid_strs};
}

[[nodiscard]] BuiltDoc build_hoa1() {
    // ACN order for first-order: (0,0) (1,-1) (1,0) (1,1).
    struct Component {
        int order;
        int degree;
    };
    constexpr std::array<Component, 4> k_components{{{0, 0}, {1, -1}, {1, 0}, {1, 1}}};

    auto doc = adm::Document::create();
    // HOA packs must be created through the dedicated factory; the generic
    // AudioPackFormat::create() rejects TypeDefinition::HOA.
    auto pf = adm::AudioPackFormatHoa::create(adm::AudioPackFormatName{"HoaPF"});
    doc->add(pf);

    std::vector<std::shared_ptr<adm::AudioTrackUid>> uids;
    std::vector<std::string> uid_strs;
    for (std::size_t i = 0; i < k_components.size(); ++i) {
        const auto& c = k_components.at(i);
        const std::string suffix = "Hoa" + std::to_string(i);
        auto cf =
            adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"CF_" + suffix}, adm::TypeDefinition::HOA);
        cf->add(adm::AudioBlockFormatHoa{adm::Order{c.order}, adm::Degree{c.degree}});
        doc->add(cf);
        pf->addReference(cf);
        uids.push_back(wire_channel(doc, cf, pf, suffix));
    }

    finish_doc(doc, uids, "Hoa1");
    uid_strs.reserve(uids.size());
    std::ranges::transform(uids, std::back_inserter(uid_strs), [](const auto& uid) {
        return adm::formatId(uid->template get<adm::AudioTrackUidId>());
    });
    return {doc, uid_strs};
}

[[nodiscard]] bool write_fixture(const BuiltDoc& built, const std::string& out_path, uint32_t seed) {
    const auto channels = static_cast<uint16_t>(built.uids.size());

    std::ostringstream xml_buf;
    adm::writeXml(xml_buf, built.doc);

    std::vector<bw64::AudioId> ids;
    ids.reserve(built.uids.size());
    for (std::size_t i = 0; i < built.uids.size(); ++i) {
        ids.emplace_back(static_cast<uint16_t>(i + 1U), built.uids[i], "", "");
    }
    auto chna = std::make_shared<bw64::ChnaChunk>(ids);
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());

    auto writer = bw64::writeFile(out_path, channels, k_sample_rate, k_bit_depth, chna, axml);
    if (!writer) {
        std::cerr << "error: cannot write " << out_path << "\n";
        return false;
    }
    const auto samples = make_signal(channels, k_frames, seed);
    writer->write(samples.data(), k_frames);
    return true;
}

void print_usage() {
    std::cerr << "usage: mr_adm_make_fixture <kind> <out.wav>\n"
              << "kinds: objects-point | objects-extent | objects-extent-multi | objects-cartesian\n"
              << "     | directspeakers | hoa\n";
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv, argv + argc);
    if (args.size() != 3) {
        print_usage();
        return 2;
    }
    const std::string_view kind = args[1];
    const std::string out_path{args[2]};

    // Distinct seeds so a mixed-up fixture is obvious rather than silently plausible.
    BuiltDoc built;
    uint32_t seed = 0;
    if (kind == "objects-point") {
        built = build_objects(false);
        seed = 0x11111111U;
    } else if (kind == "objects-extent") {
        built = build_objects(true);
        seed = 0x22222222U;
    } else if (kind == "objects-extent-multi") {
        built = build_objects(true, 3);
        seed = 0x55555555U;
    } else if (kind == "objects-cartesian") {
        built = build_objects_cartesian();
        seed = 0x66666666U;
    } else if (kind == "directspeakers") {
        built = build_direct_speakers();
        seed = 0x33333333U;
    } else if (kind == "hoa") {
        built = build_hoa1();
        seed = 0x44444444U;
    } else {
        print_usage();
        return 2;
    }

    if (!write_fixture(built, out_path, seed)) {
        return 1;
    }
    std::cout << "wrote " << out_path << " kind=" << kind << " channels=" << built.uids.size() << " frames=" << k_frames
              << " rate=" << k_sample_rate << "\n";
    return 0;
}
