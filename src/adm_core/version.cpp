#include "adm/version.h"

namespace mradm {

std::string_view version() noexcept {
    return "0.1.0";
}

std::string_view build_profile() noexcept {
#ifdef NDEBUG
    return "release";
#else
    return "debug";
#endif
}

} // namespace mradm
