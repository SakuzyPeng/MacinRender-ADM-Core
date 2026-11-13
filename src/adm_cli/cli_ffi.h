// The mradm command line, implemented in Rust (mradm-cli, run by mradm-capi).
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// Runs the command line of this process (read from the process, not from main's arguments) and
// returns the exit code.
int mradm_cli_main(void);

#ifdef __cplusplus
}
#endif
