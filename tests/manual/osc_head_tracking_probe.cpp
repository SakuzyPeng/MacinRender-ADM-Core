// A small, headless C++ API consumer, also usable to validate the platform UDP
// implementation without building the audio renderer dependency graph.
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <thread>

#include "adm/head_tracking.h"

namespace {

std::uint32_t number(std::string_view text) {
    std::uint32_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::runtime_error("expected an unsigned decimal argument");
    }
    return value;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto port = argc > 1 ? number(argv[1]) : 9000U;
        const auto seconds = argc > 2 ? number(argv[2]) : 5U;
        if (argc > 4 || port > 65535U || seconds == 0U || seconds > 60U) {
            throw std::runtime_error("usage: osc_head_tracking_probe [port 0..65535] [seconds 1..60] [source_id]");
        }
        mradm::OscHeadTrackingReceiver receiver{static_cast<std::uint16_t>(port), argc > 3 ? argv[3] : ""};
        if (const auto started = receiver.start(); !started) {
            std::cerr << started.error().message << '\n';
            return 1;
        }
        const auto bound_port = receiver.snapshot().bound_port;
        mradm::OscHeadTrackingReceiver occupied{bound_port};
        if (occupied.start()) {
            throw std::runtime_error("exclusive bind was not enforced");
        }
        std::cout << "PORT " << bound_port << '\n' << std::flush; // readiness for a subprocess-driven sender
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds{seconds};
        while (std::chrono::steady_clock::now() < end) {
            if (receiver.snapshot().state == mradm::OscHeadTrackingState::failed) {
                std::cerr << receiver.last_error() << '\n';
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        const auto result = receiver.snapshot();
        const auto report = receiver.snapshot_json();
        receiver.stop();
        if (receiver.snapshot().fresh || !occupied.start()) {
            throw std::runtime_error("stop did not clear activity and release the port");
        }
        std::cout << report << '\n';
        return result.has_pose ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
