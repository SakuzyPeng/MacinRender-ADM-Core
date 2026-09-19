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
        if (argc > 3 || port > 65535U || seconds == 0U || seconds > 60U) {
            throw std::runtime_error("usage: osc_head_tracking_probe [port 0..65535] [seconds 1..60]");
        }
        mradm::OscHeadTrackingReceiver receiver{static_cast<std::uint16_t>(port)};
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
        receiver.stop();
        if (receiver.snapshot().fresh || !occupied.start()) {
            throw std::runtime_error("stop did not clear activity and release the port");
        }
        const auto& angles = result.orientation.euler_deg;
        const double norm_squared =
            std::accumulate(result.orientation.quaternion_xyzw.begin(),
                            result.orientation.quaternion_xyzw.end(),
                            0.0,
                            [](double sum, float value) { return sum + (static_cast<double>(value) * value); });
        std::cout << "{\"state\":" << static_cast<std::int32_t>(result.state) << ",\"samples\":" << result.sequence
                  << ",\"rejected\":" << result.rejected_packets << ",\"yaw\":" << angles[0]
                  << ",\"pitch\":" << angles[1] << ",\"roll\":" << angles[2]
                  << ",\"protocol_version\":" << result.timing.protocol_version
                  << ",\"sample_time_kind\":" << result.timing.sample_time_kind
                  << ",\"source_session_id\":" << result.timing.source_session_id
                  << ",\"source_sequence\":" << result.timing.source_sequence
                  << ",\"source_received_ns\":" << result.timing.source_received_ns
                  << ",\"sample_time_ms\":" << result.timing.sample_time_ms
                  << ",\"sample_clock_epoch\":" << result.timing.sample_clock_epoch
                  << ",\"quaternion_norm_squared\":" << norm_squared << ",\"exclusive_bind_and_stop\":true}\n";
        return result.has_pose ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
