#include <iostream>
#include <string>

#include "adm/audio_io.h"
#include "adm/errors.h"

namespace {

bool check(bool condition, const char* msg) {
    if (!condition) {
        std::cerr << "FAIL: " << msg << "\n";
    }
    return condition;
}

} // namespace

int main() {
    bool ok = true;

#if MR_ADM_ENABLE_IAMF
    // The prebuilt AOM bridge is validated by integration packaging tests. This
    // unit test only proves the build selected the official path.
    ok &= check(mradm::audio::iamf_encoding_available(), "IAMF bridge build selected");
#else
    const auto res = mradm::audio::convert_to_iamf("/tmp/mr_missing_input.wav", "/tmp/mr_missing_output.iamf", "9.1.6");
    ok &= check(!mradm::audio::iamf_encoding_available(), "IAMF bridge is not available in this build");
    ok &= check(!res.has_value(), "IAMF conversion is unavailable without the official bridge");
    ok &= check(res.error().code == mradm::ErrorCode::unsupported, "IAMF disabled returns unsupported");
    ok &= check(res.error().message.find("official AOM iamf-tools bridge") != std::string::npos,
                "IAMF disabled error names bridge requirement");
#endif
    return ok ? 0 : 1;
}
