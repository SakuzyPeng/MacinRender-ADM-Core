#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

#ifndef ADM_EXE_PATH
#define ADM_EXE_PATH ""
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

int main() {
    const std::string adm_exe = "\"" ADM_EXE_PATH "\"";

    const auto fixture = write_fixture();
    const FileGuard fixture_guard{fixture};
    const std::string fix = shell_quote(fixture.string());

    bool ok = true;

    // ── adm backends ──────────────────────────────────────────────────────────
    {
        auto r = run_cmd(adm_exe + " backends");
        ok &= check(r.code == 0, "backends: exit 0");
        ok &= check(r.out.find("libear") != std::string::npos, "backends: 'libear' in output");
        ok &= check(r.out.find("0+2+0") != std::string::npos, "backends: stereo layout listed");
        ok &= check(r.out.find("wav71") != std::string::npos, "backends: wav71 layout listed");
        ok &= check(r.out.find("0+7+0") == std::string::npos, "backends: old 0+7+0 layout not listed");
    }

    // ── adm inspect <fixture> ─────────────────────────────────────────────────
    {
        auto r = run_cmd(adm_exe + " inspect " + fix);
        ok &= check(r.code == 0, "inspect: exit 0");
        ok &= check(r.out.find("48000") != std::string::npos, "inspect: sample rate 48000");
        ok &= check(r.out.find("Objects") != std::string::npos, "inspect: Objects section");
        ok &= check(r.out.find("CliTestObj") != std::string::npos, "inspect: object name");
    }

    // ── adm inspect --xml <fixture> ───────────────────────────────────────────
    {
        auto r = run_cmd(adm_exe + " inspect --xml " + fix);
        ok &= check(r.code == 0, "inspect --xml: exit 0");
        ok &= check(r.out.find("<?xml") != std::string::npos, "inspect --xml: XML declaration");
        ok &= check(r.out.find("audioObject") != std::string::npos, "inspect --xml: audioObject element");
        ok &= check(r.out.find("CliTestObj") != std::string::npos, "inspect --xml: object name in XML");
    }

    // ── adm inspect nonexistent → non-zero exit ───────────────────────────────
    {
        auto r = run_cmd(adm_exe + " inspect /nonexistent_mr_cli_test_xyz.wav");
        ok &= check(r.code != 0, "inspect nonexistent: non-zero exit");
    }

    if (ok) {
        std::cout << "adm_cli smoke test passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
