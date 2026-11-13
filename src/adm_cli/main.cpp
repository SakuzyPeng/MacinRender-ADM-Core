// The mradm command line is Rust (mradm-cli, run by mradm-capi's mradm_cli_main).
#include "cli_ffi.h"

int main() {
    return mradm_cli_main();
}
