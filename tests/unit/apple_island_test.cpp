// The Apple backend's islands (mradm_apple_islands) as the engine calls them: prepare over a taken
// scene, render with raw progress and the caller's cancellation, the monitor stream keeping the
// preparation, and the rejected arguments. macOS only.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <numbers>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "adm/errors.h"
#include "adm/progress.h"

#include "../../src/adm_apple/apple_island.h"
#include "../support/adm_fixture.h"
#include "../support/wav_fixture.h"
#include "scene_test_ffi.h"

namespace {

namespace fs = std::filesystem;

bool check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

MradmAdmText view(std::string_view s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

// One object at 30 degrees, 48 kHz mono.
void write_fixture(const fs::path& path, uint32_t frames) {
    auto doc = fixture::Document::create();
    auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"IslandCF"},
                                                  fixture::TypeDefinition::objects);
    fixture::AudioBlockFormatObjects block{
        fixture::SphericalPosition{fixture::Azimuth{30.0F}, fixture::Elevation{0.0F}}};
    block.set(fixture::JumpPosition{fixture::JumpPositionFlag{true}});
    cf->add(block);
    doc->add(cf);
    auto pf =
        fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"IslandPF"}, fixture::TypeDefinition::objects);
    pf->add_reference(cf);
    doc->add(pf);
    auto sf =
        fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{"IslandSF"}, fixture::FormatDefinition::pcm);
    sf->set_reference(cf);
    doc->add(sf);
    auto tf =
        fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{"IslandTF"}, fixture::FormatDefinition::pcm);
    tf->set_reference(sf);
    sf->add_reference(tf);
    doc->add(tf);
    auto uid = fixture::AudioTrackUid::create();
    uid->set_reference(tf);
    uid->set_reference(pf);
    doc->add(uid);
    auto obj = fixture::AudioObject::create(fixture::AudioObjectName{"IslandObject"});
    obj->add_reference(uid);
    doc->add(obj);
    auto content = fixture::AudioContent::create(fixture::AudioContentName{"IslandContent"});
    content->add_reference(obj);
    doc->add(content);
    auto prog = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"IslandProgramme"});
    prog->add_reference(content);
    doc->add(prog);
    fixture::reassign_ids(doc);
    std::ostringstream xml;
    fixture::write_xml(xml, doc);
    auto chna = std::make_shared<fixture::WaveChna>(std::vector<fixture::WaveAudioId>{
        fixture::WaveAudioId(1U, fixture::format_id(uid->get<fixture::AudioTrackUidId>()), "", "")});
    auto axml = std::make_shared<fixture::WaveAxml>(xml.str());
    auto writer = fixture::write_wave(path.string(), 1U, 48000U, 24U, chna, axml);
    std::vector<float> samples(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        samples[i] = 0.25F * std::sin(2.0F * std::numbers::pi_v<float> * 440.0F * static_cast<float>(i) / 48000.0F);
    }
    writer->write(samples.data(), frames);
}

MradmSceneHandle* import(const std::string& path) {
    MradmSceneHandle* scene = nullptr;
    uint32_t context = 0;
    std::vector<uint8_t> message(1024);
    mradm_scene_import(reinterpret_cast<const uint8_t*>(path.data()),
                       path.size(),
                       0U,
                       0U,
                       {},
                       nullptr,
                       0U,
                       &scene,
                       &context,
                       message.data(),
                       message.size());
    return scene;
}

// A caller: counts progress events and log lines, cancels after `cancel_after` events (negative:
// never), and checks the progress is the island's own (the engine maps it).
struct Caller {
    int cancel_after{-1};
    std::atomic<int> events{0};
    std::atomic<int> lines{0};
    std::atomic<bool> raw{true};
};

uint32_t cancelled(void* user) {
    const auto& c = *static_cast<Caller*>(user);
    return c.cancel_after >= 0 && c.events.load() >= c.cancel_after ? 1U : 0U;
}

void progress(void* user, const MradmEngineProgress* event) {
    auto& c = *static_cast<Caller*>(user);
    if (event->stage != static_cast<uint32_t>(mradm::RenderStage::rendering) &&
        event->stage != static_cast<uint32_t>(mradm::RenderStage::finished)) {
        c.raw = false;
    }
    ++c.events;
}

void log(void* user, uint32_t /*level*/, MradmAdmText module, MradmAdmText /*message*/) {
    if (std::string_view{reinterpret_cast<const char*>(module.data), module.len} == "apple") {
        auto& c = *static_cast<Caller*>(user);
        ++c.lines;
    }
}

MradmIslandContext context_of(Caller& caller) {
    return {&caller, &cancelled, &progress, &log};
}

MradmAppleSettings settings(const std::string& input, std::string_view layout) {
    MradmAppleSettings s{};
    s.input = view(input);
    s.layout = view(layout);
    return s;
}

} // namespace

int main() try {
    const auto dir = fs::temp_directory_path() / ("mr_adm_apple_island_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
    const auto input = (dir / "in.wav").string();
    write_fixture(input, 48000U * 30U);
    const auto* apple = mradm_apple_islands();
    bool ok = check(apple != nullptr && apple->prepare != nullptr && apple->render != nullptr &&
                        apple->open_stream != nullptr && apple->destroy != nullptr,
                    "the macOS build has the Apple islands");
    if (!ok) {
        return 1;
    }
    const auto binaural = settings(input, "binaural");

    Caller prepare_caller;
    auto prepare_context = context_of(prepare_caller);
    MradmEngineIslandReply reply{};
    MradmApplePrepared* prepared = nullptr;
    ok &= check(apple->prepare(&binaural, import(input), &prepare_context, &prepared, &reply) == 0 && reply.code == 0 &&
                    prepared != nullptr,
                "the Apple backend prepares a binaural render");

    Caller plain;
    auto plain_context = context_of(plain);
    const auto output = (dir / "out.wav").string();
    reply = {};
    ok &= check(apple->render(prepared, &binaural, view(output), 0U, 0U, 0U, &plain_context, &reply) == 0 &&
                    reply.code == 0,
                "the Apple backend renders");
    ok &= check(fs::exists(output) && fs::file_size(output) > 0U, "the render writes the output");
    ok &= check(plain.events.load() > 2 && plain.raw.load(), "the render reports its own progress");
    ok &= check(plain.lines.load() > 0, "the render's log lines reach the caller");

    Caller windowed;
    auto windowed_context = context_of(windowed);
    reply = {};
    ok &= check(
        apple->render(
            prepared, &binaural, view((dir / "window.wav").string()), 1U, 48000U, 24000U, &windowed_context, &reply) ==
                0 &&
            reply.code == 0,
        "a cached preparation renders a window");

    Caller before;
    before.cancel_after = 0;
    auto before_context = context_of(before);
    reply = {};
    ok &=
        check(apple->render(
                  prepared, &binaural, view((dir / "before.wav").string()), 0U, 0U, 0U, &before_context, &reply) == 0 &&
                  reply.code == static_cast<int32_t>(mradm::ErrorCode::cancelled),
              "a cancellation before the render is seen");

    Caller during;
    during.cancel_after = 2;
    auto during_context = context_of(during);
    reply = {};
    ok &=
        check(apple->render(
                  prepared, &binaural, view((dir / "during.wav").string()), 0U, 0U, 0U, &during_context, &reply) == 0 &&
                  reply.code == static_cast<int32_t>(mradm::ErrorCode::cancelled),
              "a cancellation while the render runs stops it");

    Caller stream_caller;
    auto stream_context = context_of(stream_caller);
    MradmEngineIslandStream stream{};
    std::vector<uint8_t> message(4096);
    std::vector<uint8_t> context(4096);
    size_t message_len = message.size();
    size_t context_len = context.size();
    const MradmEngineIslandError error{message.data(), &message_len, context.data(), &context_len};
    ok &= check(apple->open_stream(prepared, &binaural, &stream_context, &stream, &error) == 0 &&
                    stream.out_channels == 2U && stream.sample_rate == 48000U && stream.process != nullptr,
                "the Apple backend opens a monitor stream");
    apple->destroy(prepared); // The stream keeps the preparation.
    if (stream.process != nullptr) {
        std::vector<float> block(size_t{2} * 1024U);
        size_t produced = 0;
        size_t process_message_len = message.size();
        size_t process_context_len = context.size();
        const MradmEngineIslandError process_error{
            message.data(), &process_message_len, context.data(), &process_context_len};
        ok &= check(stream.process(stream.ctx, block.data(), block.size(), 1024U, &produced, &process_error) == 0 &&
                        produced == 1024U,
                    "the stream renders after its preparation handle is gone");
        stream.destroy(stream.ctx);
    }

    Caller rejected_caller;
    auto rejected_context = context_of(rejected_caller);
    const auto hoa = settings(input, "hoa3");
    MradmApplePrepared* rejected = nullptr;
    reply = {};
    ok &= check(apple->prepare(&hoa, import(input), &rejected_context, &rejected, &reply) == 0 &&
                    reply.code == static_cast<int32_t>(mradm::ErrorCode::unsupported) && rejected == nullptr,
                "an unsupported layout is rejected");
    ok &= check(apple->prepare(nullptr, nullptr, &rejected_context, &rejected, &reply) == 1,
                "NULL settings are rejected");
    ok &= check(apple->render(nullptr, &binaural, view(output), 0U, 0U, 0U, &plain_context, &reply) == 1,
                "a NULL preparation is rejected");

    std::error_code ignored;
    fs::remove_all(dir, ignored);
    if (!ok) {
        return 1;
    }
    std::cout << "apple island tests passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "apple island test exception: " << error.what() << '\n';
    return 1;
}
