#include "../support/adm_fixture.h"
// Deterministic ADM BWF fixture generator for the cross-platform numerical baseline.
//
// The baseline compares final float32 PCM across macOS / Linux / Windows. That is only
// meaningful if the *inputs* are identical too, so this generator is bit-exact by construction
// (roadmap §6.1: "不能让各平台分别用未受控的 sin() 生成输入"):
//
//   - The sample values come from a 32-bit LCG. The integer is mapped to float by subtracting
//     2^23 and dividing by 2^23. Both steps are exact in binary32 (|v| <= 2^23 converts
//     exactly, and the divisor is a power of two), so no libm and no rounding choice is
//     involved. the independent fixture writer applies the legacy 24-bit quantisation; the generated
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
//   objects-room     1 Cartesian Objects channel with equal-size extent moving over four blocks —
//                    the Triple Balance size path (PCM consistency 3.4)
//   params           40 same-start Objects blocks and a Cartesian DirectSpeakers channel, both with
//                    dB gains — block order and import projections (PCM consistency 3.5)
//   hoa3             HOA3 (16 channels) — higher-order decoding, whose AllRAD products once took
//                    nalgebra's runtime-dispatched SIMD path (PCM consistency 3.3)
//   objects-cartesian  1 Objects channel with CartesianPosition + extent — the only fixture that
//                    leaves block.position.cartesian == true, which is what distance_from_position
//                    (hoa/vbap/binaural) and mdap_spread_degrees branch on. Without it those
//                    length computations are dead code in the matrix.

#include <algorithm>
#include <array>
#include <chrono>
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

#include "../support/wav_fixture.h"

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
    std::shared_ptr<fixture::Document> doc;
    std::vector<std::string> uids; // one per audio channel, in track order
};

// Wire one channel format into the pack/stream/track/uid chain and return the uid.
// Templated on the pack type: HOA packs are AudioPackFormatHoa, and libadm resolves
// set_reference() on the concrete type, so passing the base pointer would be wrong.
template <typename PackPtr>
std::shared_ptr<fixture::AudioTrackUid> wire_channel(const std::shared_ptr<fixture::Document>& doc,
                                                     const std::shared_ptr<fixture::AudioChannelFormat>& cf,
                                                     const PackPtr& pf,
                                                     const std::string& suffix) {
    auto sf = fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{"SF_" + suffix},
                                                 fixture::FormatDefinition::pcm);
    sf->set_reference(cf);
    doc->add(sf);

    auto tf = fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{"TF_" + suffix},
                                                fixture::FormatDefinition::pcm);
    tf->set_reference(sf);
    sf->add_reference(tf);
    doc->add(tf);

    auto uid = fixture::AudioTrackUid::create();
    uid->set_reference(tf);
    uid->set_reference(pf);
    doc->add(uid);
    return uid;
}

void finish_doc(const std::shared_ptr<fixture::Document>& doc,
                const std::vector<std::shared_ptr<fixture::AudioTrackUid>>& uids,
                const std::string& name) {
    auto obj = fixture::AudioObject::create(fixture::AudioObjectName{name + "Object"});
    for (const auto& uid : uids) {
        obj->add_reference(uid);
    }
    doc->add(obj);

    auto content = fixture::AudioContent::create(fixture::AudioContentName{name + "Content"});
    content->add_reference(obj);
    doc->add(content);

    auto prog = fixture::AudioProgramme::create(fixture::AudioProgrammeName{name + "Programme"});
    prog->add_reference(content);
    doc->add(prog);

    fixture::reassign_ids(doc);
}

[[nodiscard]] BuiltDoc build_objects(bool with_extent, int track_count = 1) {
    auto doc = fixture::Document::create();
    std::vector<std::shared_ptr<fixture::AudioTrackUid>> uids;
    for (int i = 0; i < track_count; ++i) {
        const std::string suffix = track_count == 1 ? "" : std::to_string(i);
        auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"ObjCF" + suffix},
                                                      fixture::TypeDefinition::objects);
        fixture::AudioBlockFormatObjects block{fixture::SphericalPosition{
            fixture::Azimuth{30.0F - (static_cast<float>(i) * 60.0F)}, fixture::Elevation{0.0F}}};
        if (with_extent) {
            // Width exceeds the spreader gate; diffuse keeps the OLA bus active as well.
            block.set(fixture::Width{30.0F});
            block.set(fixture::Height{10.0F});
            block.set(fixture::Diffuse{0.5F});
        }
        cf->add(block);
        doc->add(cf);
        auto pf = fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"ObjPF" + suffix},
                                                   fixture::TypeDefinition::objects);
        pf->add_reference(cf);
        doc->add(pf);
        uids.push_back(wire_channel(doc, cf, pf, "Obj" + suffix));
    }
    const std::string name = with_extent ? "ObjectsExtent" : "ObjectsPoint";
    finish_doc(doc, uids, name + (track_count == 1 ? "" : "Multi"));
    std::vector<std::string> uid_strs;
    uid_strs.reserve(uids.size());
    std::ranges::transform(uids, std::back_inserter(uid_strs), [](const auto& uid) {
        return fixture::format_id(uid->template get<fixture::AudioTrackUidId>());
    });
    return {doc, uid_strs};
}

// Cartesian counterpart of build_objects(true). Coordinates are chosen so the vector length is
// exactly representable: 0.25² + 0.75² + 0.375² = 0.765625 = 0.875². A correctly rounded length
// therefore returns exactly 0x3F600000; glibc's three-argument std::hypot returns 0x3F600001.
// The case is a decidable correctness probe, not an arbitrary tie-break between implementations.
[[nodiscard]] BuiltDoc build_objects_cartesian() {
    auto doc = fixture::Document::create();
    auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"ObjCartCF"},
                                                  fixture::TypeDefinition::objects);
    fixture::AudioBlockFormatObjects block{
        fixture::CartesianPosition{fixture::X{0.25F}, fixture::Y{0.75F}, fixture::Z{0.375F}}};
    block.set(fixture::Cartesian{true});
    // Extent is required at all: distance_from_position is reached from the extent path, so a
    // point source would leave it dead even with cartesian coordinates.
    //
    // The values are normalised (0..1), which is what BS.2076 specifies for cartesian objects and
    // what mdap_spread_degrees assumes when it scales width by 60. The polar objects-extent
    // fixture carries degrees (Width 30), so there the same expression yields 1800 and
    // std::min(180, ...) saturates — the spread numerics are inert in that case. Keeping this
    // fixture normalised is what makes the VBAP spread path measurable at all.
    block.set(fixture::Width{0.5F});
    block.set(fixture::Height{0.25F});
    block.set(fixture::Depth{0.125F});
    block.set(fixture::Diffuse{0.5F});
    cf->add(block);
    doc->add(cf);

    auto pf =
        fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"ObjCartPF"}, fixture::TypeDefinition::objects);
    pf->add_reference(cf);
    doc->add(pf);

    std::vector<std::shared_ptr<fixture::AudioTrackUid>> uids{wire_channel(doc, cf, pf, "ObjCart")};
    finish_doc(doc, uids, "ObjectsCartesian");

    std::vector<std::string> uid_strs;
    uid_strs.reserve(uids.size());
    std::ranges::transform(uids, std::back_inserter(uid_strs), [](const auto& uid) {
        return fixture::format_id(uid->template get<fixture::AudioTrackUidId>());
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

    auto doc = fixture::Document::create();
    auto pf = fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"DsPF"},
                                               fixture::TypeDefinition::direct_speakers);
    doc->add(pf);

    std::vector<std::shared_ptr<fixture::AudioTrackUid>> uids;
    std::vector<std::string> uid_strs;
    for (std::size_t i = 0; i < k_speakers.size(); ++i) {
        const auto& sp = k_speakers.at(i);
        const std::string suffix = "Ds" + std::to_string(i);
        auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"CF_" + suffix},
                                                      fixture::TypeDefinition::direct_speakers);
        fixture::AudioBlockFormatDirectSpeakers block{fixture::SphericalSpeakerPosition{
            fixture::Azimuth{sp.azimuth}, fixture::Elevation{sp.elevation}, fixture::Distance{1.0F}}};
        block.add(fixture::SpeakerLabel{sp.label});
        cf->add(block);
        doc->add(cf);
        pf->add_reference(cf);
        uids.push_back(wire_channel(doc, cf, pf, suffix));
    }

    finish_doc(doc, uids, "DirectSpeakers");
    uid_strs.reserve(uids.size());
    std::ranges::transform(uids, std::back_inserter(uid_strs), [](const auto& uid) {
        return fixture::format_id(uid->template get<fixture::AudioTrackUidId>());
    });
    return {doc, uid_strs};
}

// Cartesian object for the Triple Balance room renderer, which accepts only equal-size extent
// (width = height = depth) and no diffuse. Four 250 ms blocks move the source through the room
// and change its size, so the position and size smoothing and the size lattice are exercised.
[[nodiscard]] BuiltDoc build_objects_room() {
    struct Step {
        float x;
        float y;
        float z;
        float size;
    };
    constexpr std::array<Step, 4> k_steps{{{-0.5F, 0.75F, 0.0F, 0.0F},
                                           {0.25F, 0.5F, 0.25F, 0.25F},
                                           {0.75F, -0.25F, 0.5F, 0.625F},
                                           {-0.25F, -0.75F, 0.875F, 0.125F}}};
    auto doc = fixture::Document::create();
    auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"ObjRoomCF"},
                                                  fixture::TypeDefinition::objects);
    for (std::size_t i = 0; i < k_steps.size(); ++i) {
        const auto& step = k_steps.at(i);
        fixture::AudioBlockFormatObjects block{
            fixture::CartesianPosition{fixture::X{step.x}, fixture::Y{step.y}, fixture::Z{step.z}}};
        block.set(fixture::Cartesian{true});
        block.set(fixture::Rtime{fixture::Time{std::chrono::milliseconds{250} * static_cast<int>(i)}});
        block.set(fixture::Duration{fixture::Time{std::chrono::milliseconds{250}}});
        block.set(fixture::Width{step.size});
        block.set(fixture::Height{step.size});
        block.set(fixture::Depth{step.size});
        cf->add(block);
    }
    doc->add(cf);
    auto pf =
        fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"ObjRoomPF"}, fixture::TypeDefinition::objects);
    pf->add_reference(cf);
    doc->add(pf);
    std::vector<std::shared_ptr<fixture::AudioTrackUid>> uids{wire_channel(doc, cf, pf, "ObjRoom")};
    finish_doc(doc, uids, "ObjectsRoom");
    std::vector<std::string> uid_strs;
    std::ranges::transform(uids, std::back_inserter(uid_strs), [](const auto& uid) {
        return fixture::format_id(uid->template get<fixture::AudioTrackUidId>());
    });
    return {doc, uid_strs};
}

// Parameter entry points (PCM consistency 3.5). Channel 0 is an Objects channel whose 40 blocks
// all start at zero with different directions (more than the insertion-sort threshold of every
// standard library): blocks sharing a start keep their document order and the last one is in
// effect. Both channels carry dB gains, and channel 1 is a label-free DirectSpeakers channel with
// a Cartesian position, so the import's dB conversion and Cartesian projection reach the PCM.
[[nodiscard]] BuiltDoc build_params() {
    auto doc = fixture::Document::create();
    auto objects = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"ParamsObjCF"},
                                                       fixture::TypeDefinition::objects);
    for (int i = 0; i < 40; ++i) {
        fixture::AudioBlockFormatObjects block{fixture::SphericalPosition{
            fixture::Azimuth{-97.5F + (5.0F * static_cast<float>(i))}, fixture::Elevation{i % 2 == 0 ? 0.0F : 15.0F}}};
        block.set(fixture::Rtime{fixture::Time{std::chrono::seconds{0}}});
        block.set(fixture::Duration{fixture::Time{std::chrono::seconds{1}}});
        block.set(fixture::Gain::from_db(-6.25));
        objects->add(block);
    }
    doc->add(objects);
    auto objects_pack =
        fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"ParamsObjPF"}, fixture::TypeDefinition::objects);
    objects_pack->add_reference(objects);
    doc->add(objects_pack);

    auto direct = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"ParamsDsCF"},
                                                      fixture::TypeDefinition::direct_speakers);
    fixture::AudioBlockFormatDirectSpeakers block{
        fixture::CartesianSpeakerPosition{fixture::X{0.5F}, fixture::Y{0.75F}, fixture::Z{0.25F}}};
    block.set(fixture::Gain::from_db(-3.5));
    direct->add(block);
    doc->add(direct);
    auto direct_pack = fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"ParamsDsPF"},
                                                        fixture::TypeDefinition::direct_speakers);
    direct_pack->add_reference(direct);
    doc->add(direct_pack);

    std::vector<std::shared_ptr<fixture::AudioTrackUid>> uids{wire_channel(doc, objects, objects_pack, "ParamsObj"),
                                                              wire_channel(doc, direct, direct_pack, "ParamsDs")};
    finish_doc(doc, uids, "Params");
    std::vector<std::string> uid_strs;
    std::ranges::transform(uids, std::back_inserter(uid_strs), [](const auto& uid) {
        return fixture::format_id(uid->template get<fixture::AudioTrackUidId>());
    });
    return {doc, uid_strs};
}

[[nodiscard]] BuiltDoc build_hoa(int max_order) {
    // Full ACN order up to max_order: (0,0) (1,-1) (1,0) (1,1) (2,-2) ...
    struct Component {
        int order;
        int degree;
    };
    std::vector<Component> components;
    for (int order = 0; order <= max_order; ++order) {
        for (int degree = -order; degree <= order; ++degree) {
            components.push_back({order, degree});
        }
    }

    auto doc = fixture::Document::create();
    // HOA packs must be created through the dedicated factory; the generic
    // AudioPackFormat::create() rejects TypeDefinition::hoa.
    auto pf = fixture::AudioPackFormatHoa::create(fixture::AudioPackFormatName{"HoaPF"});
    doc->add(pf);

    std::vector<std::shared_ptr<fixture::AudioTrackUid>> uids;
    std::vector<std::string> uid_strs;
    for (std::size_t i = 0; i < components.size(); ++i) {
        const auto& c = components.at(i);
        const std::string suffix = "Hoa" + std::to_string(i);
        auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"CF_" + suffix},
                                                      fixture::TypeDefinition::hoa);
        cf->add(fixture::AudioBlockFormatHoa{fixture::Order{c.order}, fixture::Degree{c.degree}});
        doc->add(cf);
        pf->add_reference(cf);
        uids.push_back(wire_channel(doc, cf, pf, suffix));
    }

    finish_doc(doc, uids, "Hoa" + std::to_string(max_order));
    uid_strs.reserve(uids.size());
    std::ranges::transform(uids, std::back_inserter(uid_strs), [](const auto& uid) {
        return fixture::format_id(uid->template get<fixture::AudioTrackUidId>());
    });
    return {doc, uid_strs};
}

void write_fixture(const BuiltDoc& built, const std::string& out_path, uint32_t seed) {
    const auto channels = static_cast<uint16_t>(built.uids.size());

    std::ostringstream xml_buf;
    fixture::write_xml(xml_buf, built.doc);

    std::vector<fixture::WaveAudioId> ids;
    ids.reserve(built.uids.size());
    for (std::size_t i = 0; i < built.uids.size(); ++i) {
        ids.emplace_back(static_cast<uint16_t>(i + 1U), built.uids[i], "", "");
    }
    auto chna = std::make_shared<fixture::WaveChna>(ids);
    auto axml = std::make_shared<fixture::WaveAxml>(xml_buf.str());

    auto writer = fixture::write_wave(out_path, channels, k_sample_rate, k_bit_depth, chna, axml);
    const auto samples = make_signal(channels, k_frames, seed);
    writer->write(samples.data(), k_frames);
}

void print_usage() {
    std::cerr << "usage: mr_adm_make_fixture <kind> <out.wav>\n"
              << "kinds: objects-point | objects-extent | objects-extent-multi | objects-cartesian\n"
              << "     | objects-room | params | directspeakers | hoa | hoa3\n";
}

} // namespace

int main(int argc, char** argv) try {
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
        built = build_hoa(1);
        seed = 0x44444444U;
    } else if (kind == "objects-room") {
        built = build_objects_room();
        seed = 0x88888888U;
    } else if (kind == "params") {
        built = build_params();
        seed = 0x99999999U;
    } else if (kind == "hoa3") {
        built = build_hoa(3);
        seed = 0x77777777U;
    } else {
        print_usage();
        return 2;
    }

    write_fixture(built, out_path, seed);
    std::cout << "wrote " << out_path << " kind=" << kind << " channels=" << built.uids.size() << " frames=" << k_frames
              << " rate=" << k_sample_rate << "\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "fixture setup failed: " << error.what() << '\n';
    return 1;
}
