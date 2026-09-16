#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#include "hptf_eq.h"

namespace {
using namespace mradm::render_common;
using mradm::HptfPreampMode;
constexpr std::uint32_t k_rate = 48000;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

HptfCoefficients design(std::string_view text, HptfPreampMode mode = HptfPreampMode::warn_only) {
    const auto profile = parse_parametric_eq(text);
    require(profile.has_value(), "valid regression profile must parse");
    const auto coeffs = design_cascade(*profile, k_rate, mode);
    require(coeffs.has_value(), "valid regression profile must design");
    return *coeffs;
}

HptfCoefficients revision_gain(std::uint64_t revision) {
    HptfCoefficients coeffs;
    coeffs.sample_rate = k_rate;
    coeffs.preamp_gain = revision % 2U != 0U ? 0.5F : 0.25F;
    coeffs.preamp_db = revision % 2U != 0U ? -6.0206F : -12.0412F;
    coeffs.max_response_db = coeffs.preamp_db;
    return coeffs;
}

void process_constant(HptfProcessor& processor, std::size_t frames, float expected = -1.0F) {
    std::vector<float> pcm(frames * 2U, 1.0F);
    processor.process(pcm.data(), frames);
    if (expected >= 0.0F) {
        require(std::ranges::all_of(pcm, [expected](float value) { return std::abs(value - expected) < 1e-6F; }),
                "all samples use the expected post-seek gain");
    }
}

void test_preamp_only_and_reset() {
    for (const auto* text : {"Preamp: -6 dB\n", "Preamp: -6 dB\nFilter 1: OFF PK Fc 1000 Hz Gain 3 dB Q 1\n"}) {
        const auto coeffs = design(text);
        require(!coeffs.is_bypass(), "preamp-only compensation is enabled");
        HptfCascade cascade;
        cascade.prepare(2);
        cascade.set_coefficients(coeffs);
        std::array<float, 4> pcm{0.5F, -0.5F, 0.25F, -0.25F};
        const auto input = pcm;
        cascade.process(pcm.data(), 2);
        for (std::size_t i = 0; i < pcm.size(); ++i) {
            require(std::abs(pcm.at(i) - (input.at(i) * std::pow(10.0, -6.0 / 20.0))) < 1e-6,
                    "preamp-only stage preserves the requested gain and channel mapping");
        }
    }
    HptfProcessor processor;
    processor.prepare(2, k_rate);
    processor.publish(revision_gain(1), 1);
    process_constant(processor, k_hptf_blend_frames);
    processor.publish(revision_gain(2), 2);
    process_constant(processor, 256);
    require(processor.active_snapshot().revision == 1, "in-flight target is not yet reported as applied");
    require(processor.active_snapshot().coefficients.preamp_db == revision_gain(1).preamp_db,
            "preamp metadata belongs to the applied revision");
    processor.reset_state();
    require(processor.active_snapshot().revision == 2, "seek adopts the in-flight target");
    process_constant(processor, 4096, 0.25F);

    processor.publish(revision_gain(3), 3);
    process_constant(processor, 256);
    processor.publish(revision_gain(4), 4);
    processor.reset_state();
    require(processor.applied_revision() == 4, "seek prefers a newer pending target over the in-flight target");
    process_constant(processor, 4096, 0.25F);
    processor.publish_bypass(5);
    processor.reset_state();
    require(processor.applied_revision() == 5 && processor.active_coefficients().is_bypass(),
            "seek adopts pending bypass and publishes matching status");
    process_constant(processor, 4096, 1.0F);
}

void test_invalid_parameters() {
    for (const auto* text : {"Preamp: abc 6 dB\n",
                             "Filter 1: ON PK Fc 1000junk Hz Gain 3 dB Q 1\n",
                             "Filter 1: ON PK Fc 1000 Hz Gain abc dB Q 1\n",
                             "Filter 1: ON PK Fc 1000 Hz Gain nan dB Q 1\n",
                             "Filter 1: ON PK Fc 1000 Hz Gain 3 dB Q abc\n",
                             "Filter 1: ON PK Fc 1000 Hz Gain 3 dB Q\n",
                             "not a profile\n"}) {
        require(!parse_parametric_eq(text), "malformed explicit parameters must fail, not use defaults");
    }
    for (const auto* text : {"Preamp: 800 dB\n",
                             "Preamp: -800 dB\n",
                             "Filter 1: ON PK Fc 1000 Hz Gain 1e308 dB Q 1\n",
                             "Filter 1: ON PK Fc 1e-30 Hz Gain 3 dB Q 1\n",
                             "Filter 1: ON PK Fc 1000 Hz Gain 3 dB Q 1e-300\n"}) {
        const auto profile = parse_parametric_eq(text);
        require(profile.has_value(), "finite input reaches coefficient validation");
        require(!design_cascade(*profile, k_rate, HptfPreampMode::warn_only),
                "overflowing or unstable quantised coefficients must be rejected");
    }
    auto profile = *parse_parametric_eq("Filter 1: ON PK Fc 1000 Hz Gain 3 dB Q 1\n");
    profile.bands[0].q = std::numeric_limits<double>::quiet_NaN();
    require(!design_cascade(profile, k_rate, HptfPreampMode::warn_only), "in-memory profiles are validated too");
    profile.bands[0].enabled = false;
    require(!design_cascade(profile, k_rate, HptfPreampMode::warn_only), "disabled bands are validated consistently");
    profile.bands[0].q = 1.0;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): deliberately invalid public input.
    profile.bands[0].type = static_cast<HptfBandType>(255);
    require(!design_cascade(profile, k_rate, HptfPreampMode::warn_only), "unknown in-memory band types are rejected");
}

void test_narrow_peak_trim() {
    const std::array profiles{"Filter 1: ON PK Fc 1000 Hz Gain 12 dB Q 80\n",
                              "Filter 1: ON PK Fc 1093.17 Hz Gain 24 dB Q 300\n",
                              ("Filter 1: ON PK Fc 960 Hz Gain 8 dB Q 50\nFilter 2: ON PK Fc 1000 Hz Gain 12 dB Q 50\n"
                               "Filter 3: ON NO Fc 1004 Hz Q 80\n"),
                              "Filter 1: ON LSC Fc 230 Hz Gain 9 dB Q 3\nFilter 2: ON HSC Fc 8500 Hz Gain 6 dB Q 2\n",
                              "Filter 1: ON LP Fc 4100 Hz Q 15\n"};
    for (const auto* text : profiles) {
        const auto coeffs = design(text, HptfPreampMode::auto_trim);
        require(coeffs.max_response_db <= 0.001F, "auto-trim reports a non-positive upper bound");
        for (int i = 0; i < 8191; ++i) {
            // Offset grid with a different cardinality from the implementation and old tests.
            const double hz = 20.0 * std::pow(1000.0, (static_cast<double>(i) + 0.37) / 8191.0);
            require(cascade_magnitude_db(coeffs, hz) <= 0.002, "auto-trim covers off-grid and overlapping peaks");
        }
        for (const double hz : {1000.0, 1093.17, 960.0, 4100.0}) {
            require(cascade_magnitude_db(coeffs, hz) <= 0.002, "auto-trim includes narrow-band centers");
        }
    }
    // Independently evaluate the RUNNING filter at the originally missed 1 kHz peak.
    const auto coeffs = design(profiles.front(), HptfPreampMode::auto_trim);
    HptfCascade cascade;
    cascade.prepare(1);
    cascade.set_coefficients(coeffs);
    std::vector<float> impulse(65536, 0.0F);
    impulse[0] = 1.0F;
    cascade.process(impulse.data(), impulse.size());
    std::complex<double> response{};
    for (std::size_t i = 0; i < impulse.size(); ++i) {
        const double phase = -2.0 * std::acos(-1.0) * 1000.0 * static_cast<double>(i) / k_rate;
        response += static_cast<double>(impulse[i]) * std::complex<double>{std::cos(phase), std::sin(phase)};
    }
    require(std::abs(response) <= 1.0002 && std::abs(response) > 0.99, "actual narrow-band gain is trimmed accurately");
}

void test_mailbox_publication() {
    HptfMailbox mailbox;
    constexpr std::uint64_t k_updates = 20000;
    std::thread writer([&] {
        for (std::uint64_t revision = 1; revision <= k_updates; ++revision) {
            HptfSnapshot snapshot;
            snapshot.revision = revision;
            snapshot.coefficients.sample_rate = static_cast<std::uint32_t>(revision);
            for (auto& section : snapshot.coefficients.sections) {
                section.b0 = static_cast<float>(revision);
                section.a2 = -static_cast<float>(revision);
            }
            mailbox.publish(snapshot);
        }
    });
    std::uint64_t seen = 0;
    bool coherent = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (seen != k_updates && std::chrono::steady_clock::now() < deadline) {
        if (const auto snapshot = mailbox.consume()) {
            coherent &= snapshot->revision > seen && snapshot->coefficients.sample_rate == snapshot->revision;
            for (const auto& section : snapshot->coefficients.sections) {
                coherent &= section.b0 == static_cast<float>(snapshot->revision) &&
                            section.a2 == -static_cast<float>(snapshot->revision);
            }
            seen = snapshot->revision;
        }
    }
    writer.join();
    require(coherent && seen == k_updates, "publication never tears a snapshot and retains the final update");
}

void test_quantised_low_frequency_peaks() {
    for (const double fc : {20.1, 30.7, 46.3, 98.73, 999.13, 17777.77}) {
        for (const double q : {0.37, 8.0, 80.0, 300.0}) {
            HptfProfile profile;
            profile.bands.push_back({HptfBandType::peaking, true, fc, 12.0, q});
            const auto result = design_cascade(profile, k_rate, HptfPreampMode::auto_trim);
            require(result.has_value(), "quantised narrow-band filter remains stable");
            const double width = std::max(8.0 * fc / q, 1.0);
            const double low = std::max(20.0, fc - width);
            const double high = std::min(20000.0, fc + width);
            for (int i = 0; i <= 2048; ++i) {
                const double hz = low + ((high - low) * static_cast<double>(i) / 2048.0);
                const auto response = cascade_magnitude_db(*result, hz);
                if (response > 0.002) {
                    std::cerr << "fc=" << fc << " Q=" << q << " hz=" << hz << " gain=" << response << '\n';
                }
                require(response <= 0.002, "trim covers peak shifts caused by low-frequency coefficient quantisation");
            }
        }
    }
}

void test_concurrent_processing_and_queries() {
    HptfProcessor processor;
    processor.prepare(2, k_rate);
    std::atomic<bool> stop{false};
    std::atomic<bool> coherent{true};
    std::thread audio([&] {
        std::array<float, 512> pcm{};
        while (!stop.load()) {
            pcm.fill(0.1F);
            processor.process(pcm.data(), pcm.size() / 2U);
        }
    });
    std::thread query([&] {
        while (!stop.load()) {
            const auto snapshot = processor.active_snapshot();
            if (snapshot.revision != 0) {
                const auto expected = revision_gain(snapshot.revision);
                if (snapshot.coefficients.preamp_gain != expected.preamp_gain ||
                    snapshot.coefficients.preamp_db != expected.preamp_db ||
                    snapshot.coefficients.max_response_db != expected.max_response_db) {
                    coherent.store(false);
                }
            }
        }
    });
    constexpr std::uint64_t k_updates = 500;
    for (std::uint64_t revision = 1; revision <= k_updates; ++revision) {
        processor.publish(revision_gain(revision), revision);
        if (revision % 8U == 0U) {
            std::this_thread::yield();
        }
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (processor.applied_revision() != k_updates && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    stop.store(true);
    audio.join();
    query.join();
    require(coherent.load() && processor.applied_revision() == k_updates,
            "concurrent queries return one applied revision, and rapid updates reach the final target");
}
} // namespace

int main() {
    try {
        test_preamp_only_and_reset();
        test_invalid_parameters();
        test_narrow_peak_trim();
        test_quantised_low_frequency_peaks();
        test_mailbox_publication();
        test_concurrent_processing_and_queries();
        std::cout << "HpTF regression tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
