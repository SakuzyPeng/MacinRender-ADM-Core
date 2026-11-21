// The live override resolution and the live-gain de-zipper of the Apple island (render_common.h).
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "../support/rust_services_test.h"
#include "render_common.h"

namespace {

bool check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

bool verify_live_head_locked_precedence() {
    mradm::LiveOverrides overrides;

    mradm::LiveObjectOverride whole;
    whole.object_id = "AO_bed";
    whole.head_locked = true;
    overrides.objects.push_back(whole);

    // A gain-only channel entry must not mask the object's explicit head lock.
    mradm::LiveObjectOverride gain_only;
    gain_only.object_id = "AO_bed";
    gain_only.speaker_label = "M+030";
    gain_only.gain_db = -6.0F;
    overrides.objects.push_back(gain_only);
    const auto inherited = mradm::render_common::resolve_live_head_locked(overrides, "AO_bed", "M+030");

    // Once the channel supplies an explicit value, it wins over the object.
    mradm::LiveObjectOverride channel;
    channel.object_id = "AO_bed";
    channel.speaker_label = "M+030";
    channel.head_locked = false;
    overrides.objects.push_back(channel);
    const auto explicit_channel = mradm::render_common::resolve_live_head_locked(overrides, "AO_bed", "M+030");
    const auto other_channel = mradm::render_common::resolve_live_head_locked(overrides, "AO_bed", "M-030");

    mradm::LiveOverrides gain_only_set;
    gain_only_set.objects.push_back(gain_only);
    const auto absent = mradm::render_common::resolve_live_head_locked(gain_only_set, "AO_bed", "M+030");

    bool ok = check(inherited.has_value() && *inherited,
                    "gain-only channel override inherits explicit whole-object head lock");
    ok &= check(explicit_channel.has_value() && !*explicit_channel,
                "explicit channel head lock wins over whole-object value");
    ok &= check(other_channel.has_value() && *other_channel, "unmatched channel keeps whole-object head lock");
    ok &= check(!absent.has_value(), "gain-only override leaves head lock inherited from ADM");
    return ok;
}

mradm::LiveObjectOverride gain_override(std::string_view label, float gain_db, bool mute = false) {
    mradm::LiveObjectOverride entry;
    entry.object_id = "AO_bed";
    entry.speaker_label = std::string{label};
    entry.gain_db = gain_db;
    entry.mute = mute;
    return entry;
}

// The live gain resolver of the island renderers (moved from the C++ ear_render_fixture_test).
bool verify_live_channel_gain() {
    using mradm::render_common::resolve_live_channel_gain;
    mradm::LiveOverrides ov;
    ov.objects.push_back(gain_override("M+030", -6.0206F));
    ov.objects.push_back(gain_override("", -20.0F));
    const auto m030 = resolve_live_channel_gain(ov, "AO_bed", "M+030");
    bool ok = check(m030.has_value() && std::fabs(*m030 - 0.5F) < 1.0e-3F, "per-channel override wins (~0.5)");
    const auto other = resolve_live_channel_gain(ov, "AO_bed", "M-030");
    ok &= check(other.has_value() && std::fabs(*other - 0.1F) < 1.0e-3F, "falls back to the whole object (~0.1)");
    ok &= check(!resolve_live_channel_gain(ov, "AO_other", "M+030").has_value(), "unrelated object has no override");

    mradm::LiveOverrides plus_only;
    plus_only.objects.push_back(gain_override("M+030", -6.0206F));
    ok &= check(!resolve_live_channel_gain(plus_only, "AO_bed", "M-030").has_value(),
                "a channel override does not bleed onto another channel");
    mradm::LiveOverrides messy;
    messy.objects.push_back(gain_override("m_+030", -6.0206F));
    const auto canonical = resolve_live_channel_gain(messy, "AO_bed", "M+030");
    ok &= check(canonical.has_value() && std::fabs(*canonical - 0.5F) < 1.0e-3F, "speaker labels are canonicalised");
    mradm::LiveOverrides muted;
    muted.objects.push_back(gain_override("M+030", 12.0F, true));
    const auto zero = resolve_live_channel_gain(muted, "AO_bed", "M+030");
    ok &= check(zero.has_value() && *zero == 0.0F, "mute is an exact zero multiplier");
    ok &= check(!resolve_live_channel_gain(muted, "AO_bed", "M-030").has_value(),
                "a channel mute does not affect another channel");
    return ok;
}

// The island renderers' live-gain de-zipper (moved from the C++ ear_render_fixture_test).
bool verify_live_gain_ramp() {
    mradm::render_common::LiveGainRamp ramp(1000U, 4U);
    ramp.set_target(0.5F);
    bool ok = check(std::fabs(ramp.next() - 0.5F) < 1.0e-6F, "an initial target is immediate");
    ramp.set_target(1.0F);
    const std::array<float, 4> rising{ramp.next(), ramp.next(), ramp.next(), ramp.next()};
    ok &= check(std::fabs(rising[0] - 0.5F) < 1.0e-6F, "an edit starts at the old value");
    ok &= check(std::fabs(rising[1] - (2.0F / 3.0F)) < 1.0e-6F, "first interpolated sample");
    ok &= check(std::fabs(rising[2] - (5.0F / 6.0F)) < 1.0e-6F, "second interpolated sample");
    ok &= check(std::fabs(rising[3] - 1.0F) < 1.0e-6F, "the ramp reaches its target");
    ramp.set_target(0.0F);
    (void) ramp.next();
    (void) ramp.next();
    ramp.set_target(0.5F);
    ok &= check(std::fabs(ramp.next() - (1.0F / 3.0F)) < 1.0e-6F, "a retarget keeps the in-flight value");
    return ok;
}

} // namespace

int main() {
    mradm_test::bind_rust_services();
    const bool ok = verify_live_head_locked_precedence() && verify_live_channel_gain() && verify_live_gain_ramp();
    if (ok) {
        std::cout << "live override test passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
