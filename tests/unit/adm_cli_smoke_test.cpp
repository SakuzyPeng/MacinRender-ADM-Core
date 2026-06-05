#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <adm/adm.hpp>
#include <adm/utilities/id_assignment.hpp>
#include <adm/write.hpp>
#include <bw64/bw64.hpp>
#include <sys/wait.h>

#ifndef MRADM_EXE_PATH
#define MRADM_EXE_PATH ""
#endif

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

struct RunResult {
    int code{-1};
    std::string out;
};

RunResult run_cmd(const std::string& cmd) {
    // NOLINTNEXTLINE(cert-env33-c)
    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (pipe == nullptr) {
        return {};
    }
    std::string out;
    std::array<char, 512> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        out += buf.data();
    }
    const int status = pclose(pipe);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, std::move(out)};
}

bool check(bool condition, const char* msg) {
    if (!condition) {
        std::cerr << "FAIL: " << msg << "\n";
    }
    return condition;
}

std::string shell_quote(const std::string& value) {
    std::string quoted{"'"};
    for (const char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted += ch;
        }
    }
    quoted += "'";
    return quoted;
}

// Minimal Objects BW64 fixture: 1 channel, az=0 el=0, no audio samples.
std::filesystem::path write_fixture() {
    auto doc = adm::Document::create();

    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"CF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
        cf->add(block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"PF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"SF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"TF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto obj = adm::AudioObject::create(adm::AudioObjectName{"CliTestObj"});
    obj->addReference(uid);
    doc->add(obj);

    auto content = adm::AudioContent::create(adm::AudioContentName{"CliTestContent"});
    content->addReference(obj);
    doc->add(content);

    auto prog = adm::AudioProgramme::create(adm::AudioProgrammeName{"CliTestProg"});
    prog->addReference(content);
    doc->add(prog);

    adm::reassignIds(doc);

    std::ostringstream xml_buf;
    adm::writeXml(xml_buf, doc);
    const std::string uid_str = adm::formatId(uid->get<adm::AudioTrackUidId>());

    auto path = std::filesystem::temp_directory_path() / "mr_adm_cli_smoke_fixture.wav";
    auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1U, uid_str, "", "")});
    auto axml = std::make_shared<bw64::AxmlChunk>(xml_buf.str());
    auto writer = bw64::writeFile(path.string(), 1U, 48000U, 24U, chna, axml);
    (void) writer;

    return path;
}

} // namespace

// NOLINTNEXTLINE(readability-function-size): this is a linear CLI smoke script.
int main() {
    const std::string mradm_exe = "\"" MRADM_EXE_PATH "\"";

    const auto fixture = write_fixture();
    const FileGuard fixture_guard{fixture};
    const std::string fix = shell_quote(fixture.string());

    bool ok = true;

    // ── mradm backends ────────────────────────────────────────────────────────
    {
        auto r = run_cmd(mradm_exe + " backends");
        ok &= check(r.code == 0, "backends: exit 0");
        ok &= check(r.out.find("libear") != std::string::npos, "backends: 'libear' in output");
        ok &= check(r.out.find("saf-binaural-hrtf") != std::string::npos, "backends: SAF binaural backend listed");
        ok &= check(r.out.find("stereo") == std::string::npos, "backends: speaker stereo layout not listed");
        ok &= check(r.out.find("binaural") != std::string::npos, "backends: binaural layout listed");
        ok &= check(r.out.find("7.1") != std::string::npos, "backends: 7.1 layout listed");
        ok &= check(r.out.find("ChannelLock:    yes") != std::string::npos, "backends: channelLock flag listed");
        ok &= check(r.out.find("Divergence:     yes") != std::string::npos, "backends: divergence flag listed");
        ok &= check(r.out.find("ScreenRef:      no") != std::string::npos, "backends: screenRef unsupported listed");
        ok &= check(r.out.find("Diffuse:        yes") != std::string::npos, "backends: diffuse flag listed");
        ok &= check(r.out.find("0+7+0") == std::string::npos, "backends: old 0+7+0 layout not listed");
        ok &= check(r.out.find("4+7+0") == std::string::npos, "backends: old 4+7+0 layout not listed");
    }

    // ── mradm formats lists output containers + constraints ──────────────────
    {
        auto r = run_cmd(mradm_exe + " formats");
        ok &= check(r.code == 0, "formats: exit 0");
        ok &= check(r.out.find("Build features:") != std::string::npos, "formats: build features header");
        ok &= check(r.out.find("wav") != std::string::npos && r.out.find("flac") != std::string::npos &&
                        r.out.find("opus_mka") != std::string::npos && r.out.find("apac") != std::string::npos &&
                        r.out.find("iamf") != std::string::npos,
                    "formats: all containers listed");
        ok &= check(r.out.find("channels: up to 8") != std::string::npos, "formats: FLAC 8-channel cap shown");
        ok &= check(r.out.find("bitrate (per channel): 6-320 kbps") != std::string::npos,
                    "formats: Opus per-channel bitrate range shown");
        ok &= check(r.out.find("iamf_mp4") != std::string::npos, "formats: iamf_mp4 container listed");
    }

    // ── mradm render help exposes peak makeup ────────────────────────────────
    {
        auto r = run_cmd(mradm_exe + " render --help");
        ok &= check(r.code == 0, "render --help: exit 0");
        ok &= check(r.out.find("--peak-normalize-to-limit") != std::string::npos,
                    "render --help: peak normalize option listed");
        ok &= check(r.out.find("--final-gain-db") != std::string::npos, "render --help: final gain option listed");
        ok &=
            check(r.out.find("--semantic-policy") != std::string::npos, "render --help: semantic policy option listed");
        ok &= check(r.out.find("--write-semantic-report") != std::string::npos,
                    "render --help: semantic report option listed");
        ok &= check(r.out.find("--speaker-spread-mode") != std::string::npos,
                    "render --help: speaker-spread-mode option listed");
        ok &= check(r.out.find("--binaural-spread-mode") != std::string::npos,
                    "render --help: binaural-spread-mode option listed");
        ok &= check(r.out.find("saf-binaural") != std::string::npos, "render --help: saf-binaural renderer listed");
        ok &= check(r.out.find("--apple-spatial-preset") != std::string::npos,
                    "render --help: apple-spatial-preset option listed");
    }

    // ── spread mode parse: valid values accepted, invalid rejected ────────────
    {
        // --speaker-spread-mode: valid values
        for (const auto* val : {"auto", "none", "mdap"}) {
            auto r = run_cmd(mradm_exe + " render --help --speaker-spread-mode " + val);
            const std::string msg_spk = std::string("render --speaker-spread-mode ") + val + ": exit 0";
            ok &= check(r.code == 0, msg_spk.c_str());
        }
        // --speaker-spread-mode: invalid value
        {
            auto r = run_cmd(mradm_exe + " render --speaker-spread-mode invalid_xyz");
            ok &= check(r.code != 0, "render --speaker-spread-mode invalid: non-zero exit");
        }
        // --binaural-spread-mode: valid values
        for (const auto* val : {"auto", "none", "cloud", "saf-spreader"}) {
            auto r = run_cmd(mradm_exe + " render --help --binaural-spread-mode " + val);
            const std::string msg_bin = std::string("render --binaural-spread-mode ") + val + ": exit 0";
            ok &= check(r.code == 0, msg_bin.c_str());
        }
        // --binaural-spread-mode: invalid value
        {
            auto r = run_cmd(mradm_exe + " render --binaural-spread-mode invalid_xyz");
            ok &= check(r.code != 0, "render --binaural-spread-mode invalid: non-zero exit");
        }
        // --apple-spatial-preset: valid values
        for (const auto* val : {"off", "headphone-default", "headphone-movie"}) {
            auto r = run_cmd(mradm_exe + " render --help --apple-spatial-preset " + val);
            const std::string msg = std::string("render --apple-spatial-preset ") + val + ": exit 0";
            ok &= check(r.code == 0, msg.c_str());
        }
        // --apple-spatial-preset: invalid value
        {
            auto r = run_cmd(mradm_exe + " render --apple-spatial-preset invalid_xyz");
            ok &= check(r.code != 0, "render --apple-spatial-preset invalid: non-zero exit");
        }
    }

    // ── --speaker-spread-mode none renders a 5.1 mix without crashing ────────
    {
        const auto out = std::filesystem::temp_directory_path() / "mr_adm_cli_speaker_spread_none.flac";
        const FileGuard out_guard{out};
        auto r = run_cmd(mradm_exe +
                         " render --renderer saf --output-layout 5.1 "
                         "--speaker-spread-mode none -o " +
                         shell_quote(out.string()) + " -i " + fix);
        ok &= check(r.code == 0, "render --speaker-spread-mode none (5.1): exit 0");
    }

    // ── --binaural-spread-mode none renders SAF binaural without crashing ─────
    {
        const auto out = std::filesystem::temp_directory_path() / "mr_adm_cli_saf_binaural_spread_none.flac";
        const FileGuard out_guard{out};
        auto r = run_cmd(mradm_exe +
                         " render --renderer saf-binaural --binaural-spread-mode none "
                         "-o " +
                         shell_quote(out.string()) + " -i " + fix);
        ok &= check(r.code == 0, "render --binaural-spread-mode none: exit 0");
    }

    // ── legacy --renderer binaural remains accepted but warns ────────────────
    {
        const auto out = std::filesystem::temp_directory_path() / "mr_adm_cli_legacy_binaural.flac";
        const FileGuard out_guard{out};
        auto r = run_cmd(mradm_exe +
                         " render --renderer binaural --binaural-spread-mode none "
                         "-o " +
                         shell_quote(out.string()) + " -i " + fix);
        ok &= check(r.code == 0, "render --renderer binaural legacy alias: exit 0");
        ok &= check(r.out.find("legacy alias for --renderer saf-binaural") != std::string::npos,
                    "render --renderer binaural legacy alias: warning emitted");
    }

    // ── mradm inspect can write an editable semantic policy template ─────────
    {
        const auto template_path = std::filesystem::temp_directory_path() / "mr_adm_cli_semantic_policy_template.json";
        const FileGuard template_guard{template_path};
        auto r = run_cmd(mradm_exe + " inspect " + fix + " --write-semantic-policy-template " +
                         shell_quote(template_path.string()));
        ok &= check(r.code == 0, "inspect --write-semantic-policy-template: exit 0");
        std::ifstream in(template_path);
        const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        ok &= check(json.find(R"("schema": "mradm.semantic-policy.v1")") != std::string::npos,
                    "semantic template: schema present");
        ok &= check(json.find("\"global\"") != std::string::npos, "semantic template: global section present");
        ok &= check(json.find("\"CliTestObj\"") != std::string::npos, "semantic template: object name present");
        ok &= check(json.find("\"diffuse\"") != std::string::npos, "semantic template: diffuse section present");
    }

    // ── mradm layouts requires format and reports final container order ──────
    {
        auto r = run_cmd(mradm_exe + " layouts");
        ok &= check(r.code != 0, "layouts without --format: non-zero exit");
    }
    {
        auto r = run_cmd(mradm_exe + " layouts --format apac --layout 7.1");
        ok &= check(r.code == 0, "layouts --format apac --layout 7.1: exit 0");
        ok &= check(r.out.find("AudioUnit_7_1") != std::string::npos, "layouts apac 7.1: AudioUnit tag");
        ok &= check(r.out.find("L R C LFE Ls Rs Rls Rrs") != std::string::npos, "layouts apac 7.1: final APAC order");
    }
    {
        auto r = run_cmd(mradm_exe + " layouts --format wav --layout wav71");
        ok &= check(r.code == 0, "layouts --format wav --layout wav71: exit 0");
        ok &= check(r.out.find("L R C LFE Rls Rrs Ls Rs") != std::string::npos, "layouts wav 7.1: final WAV order");
    }
    {
        auto r = run_cmd(mradm_exe + " layouts --format flac --renderer saf");
        ok &= check(r.code == 0, "layouts --format flac --renderer saf: exit 0");
        ok &= check(r.out.find("Renderer: saf-vbap") != std::string::npos, "layouts renderer filter: saf heading");
        ok &= check(r.out.find("7.1.4") == std::string::npos, "layouts flac+saf: 7.1.4 hidden");
    }
    {
        auto r = run_cmd(mradm_exe + " layouts --format flac --renderer ear");
        ok &= check(r.code == 0, "layouts --format flac --renderer ear: exit 0");
        ok &= check(r.out.find("5.1.2") == std::string::npos, "layouts flac+ear: 5.1.2 hidden");
    }
    {
        auto r = run_cmd(mradm_exe + " layouts --format wav --renderer ear");
        ok &= check(r.code == 0, "layouts --format wav --renderer ear: exit 0");
        ok &= check(r.out.find("Renderer: libear") != std::string::npos, "layouts renderer filter: ear heading");
        ok &= check(r.out.find("9.1.4") != std::string::npos, "layouts wav+ear: 9.1.4 listed");
        ok &= check(r.out.find("9.1.6") != std::string::npos, "layouts wav+ear: 9.1.6 listed");
    }
    {
        auto r = run_cmd(mradm_exe + " layouts --format apac --renderer saf --layout 5.1.2");
        ok &= check(r.code != 0, "layouts apac+saf unsupported layout: non-zero exit");
        ok &= check(r.out.find("not supported") != std::string::npos, "layouts apac+saf unsupported layout: message");
    }

    // ── mradm inspect <fixture> ───────────────────────────────────────────────
    {
        auto r = run_cmd(mradm_exe + " inspect " + fix);
        ok &= check(r.code == 0, "inspect: exit 0");
        ok &= check(r.out.find("48000") != std::string::npos, "inspect: sample rate 48000");
        ok &= check(r.out.find("Objects") != std::string::npos, "inspect: Objects section");
        ok &= check(r.out.find("CliTestObj") != std::string::npos, "inspect: object name");
    }

    // ── mradm inspect --xml <fixture> ─────────────────────────────────────────
    {
        auto r = run_cmd(mradm_exe + " inspect --xml " + fix);
        ok &= check(r.code == 0, "inspect --xml: exit 0");
        ok &= check(r.out.find("<?xml") != std::string::npos, "inspect --xml: XML declaration");
        ok &= check(r.out.find("audioObject") != std::string::npos, "inspect --xml: audioObject element");
        ok &= check(r.out.find("CliTestObj") != std::string::npos, "inspect --xml: object name in XML");
    }

    // ── mradm inspect nonexistent → non-zero exit ─────────────────────────────
    {
        auto r = run_cmd(mradm_exe + " inspect /nonexistent_mr_cli_test_xyz.wav");
        ok &= check(r.code != 0, "inspect nonexistent: non-zero exit");
    }

    // ── mradm render --sofa nonexistent → non-zero exit ──────────────────────
    {
        const auto out = std::filesystem::temp_directory_path() / "mr_adm_cli_binaural_sofa_missing.wav";
        const FileGuard out_guard{out};
        auto r = run_cmd(mradm_exe + " render --renderer saf-binaural --sofa /nonexistent_mr_cli_test_xyz.sofa -o " +
                         shell_quote(out.string()) + " " + fix);
        ok &= check(r.code != 0, "render --sofa nonexistent: non-zero exit");
    }

    // ── mradm render default 2ch → binaural succeeds ─────────────────────────
    {
        const auto out = std::filesystem::temp_directory_path() / "mr_adm_cli_default_binaural.wav";
        const FileGuard out_guard{out};
        auto r = run_cmd(mradm_exe + " render -o " + shell_quote(out.string()) + " -i " + fix);
        ok &= check(r.code == 0, "render default 2ch uses binaural: exit 0");
    }

    // ── mradm render speaker stereo is disabled ──────────────────────────────
    {
        const auto out = std::filesystem::temp_directory_path() / "mr_adm_cli_speaker_stereo.wav";
        const FileGuard out_guard{out};
        auto r = run_cmd(mradm_exe + " render --renderer saf --output-layout stereo -o " + shell_quote(out.string()) +
                         " -i " + fix);
        ok &= check(r.code != 0, "render --renderer saf --output-layout stereo: non-zero exit");
    }

    // ── mradm render accepts common layout names → exit 0 ────────────────────
    {
        const auto out = std::filesystem::temp_directory_path() / "mr_adm_cli_layout_alias.wav";
        const FileGuard out_guard{out};
        auto r = run_cmd(mradm_exe + " render --renderer saf --output-layout 7.1.4 -o " + shell_quote(out.string()) +
                         " -i " + fix);
        ok &= check(r.code == 0, "render --output-layout 7.1.4: exit 0");
    }

    // ── HOA loudness: measured via 7.1.4 AllRAD decode; normalisation no longer skipped ──
    {
        const auto out = std::filesystem::temp_directory_path() / "mr_adm_cli_hoa_loudness.wav";
        const FileGuard out_guard{out};
        auto r = run_cmd(mradm_exe + " render --renderer hoa --output-layout hoa3 --loudness-target -23 -o " +
                         shell_quote(out.string()) + " -i " + fix);
        ok &= check(r.code == 0, "render HOA with loudness target: exit 0");
        ok &= check(r.out.find("loudness normalization skipped") == std::string::npos,
                    "render HOA with loudness target: no skip warning");
    }

    if (ok) {
        std::cout << "adm_cli smoke test passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
