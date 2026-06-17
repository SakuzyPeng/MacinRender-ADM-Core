#include <cstdio>
#include <cstdlib>
#include <string>

#include "audio_io_internal.h"
#include "commands.h"

CLI::App* add_apac_encode_command(CLI::App& app, ApacEncodeCliOptions& opts) {
    auto* cmd = app.add_subcommand("__apac-encode", "(internal) APAC encode worker with heartbeat protocol");
    cmd->group(""); // hidden from --help / usage
    cmd->add_option("--in", opts.input, "input float32 WAV (48 kHz)")->required();
    cmd->add_option("--out", opts.output, "output APAC path")->required();
    cmd->add_option("--layout", opts.layout, "layout id")->required();
    cmd->add_option("--bitrate", opts.bitrate, "total target bitrate kbps (0 = layout default)");
    cmd->add_option("--drc", opts.drc_music, "DRC profile: 1 = Music, 0 = None");
    cmd->add_option("--container", opts.container, "container: mpeg4 | caf");
    return cmd;
}

int run_apac_encode(const ApacEncodeCliOptions& opts) {
    const bool caf = opts.container == "caf";
    auto result =
        mradm::audio::run_apac_encode_child(opts.input, opts.output, opts.layout, opts.bitrate, opts.drc_music, caf);
    if (!result.has_value()) {
        // Emit the "E <code> <message>" protocol line on stdout so the parent's
        // watchdog can reconstruct the mradm::Error. Heartbeat (P/F) lines were
        // already streamed by the encoder during the run.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        std::printf("E %d %s\n", static_cast<int>(result.error().code), result.error().message.c_str());
        std::fflush(stdout);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
