// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// System effects of both engine models driven through the XG effect RAM: input-gain ramps,
// chorus return law, unsupported types, the sleep gate and the RAM's type-change reload.
#include <doctest/doctest.h>

#include <cmath>
#include <functional>
#include <memory>

#include "fx/fx_system.hpp"
#include "syxg2006le/effects.hpp"
#include "syxg50/effects.hpp"
#include "midi/xg_ram.hpp"
#include "test_support.hpp"

using namespace hvyxg;

namespace {

struct Rig {
    midi::xg::EffectData ram{test::tables().get()};
    std::unique_ptr<SystemEffects> fx;
    std::size_t block;
    Buses buses;

    explicit Rig(bool syxg50) : block(syxg50 ? 128 : 64) {
        ram.reset();
        ModuleContext ctx;
        ctx.tables = test::tables().get();
        ctx.shared.effect1 = &ram;
        ctx.sample_rate = 44100.0f;
        fx = syxg50 ? syxg50::make_system_effects(ctx)
                    : syxg2006le::make_system_effects(ctx);
    }
    const fx::SystemFx& sys() const { return dynamic_cast<const fx::SystemFx&>(*fx); }
    void set(std::uint8_t addr, std::uint8_t value) { ram.write(addr, value); }
    /// Run `seconds`; `feed` fills the send buses of each block (frame offset given).
    double run(float seconds, const std::function<void(Buses&, std::size_t)>& feed = {}) {
        double energy = 0;
        const auto n = static_cast<std::size_t>(seconds * 44100.0f);
        for (std::size_t t = 0; t < n; t += block) {
            buses.clear();
            if (feed) feed(buses, t);
            fx->process(buses, block);
            for (std::size_t k = 0; k < block; ++k)
                energy += double(buses.dry_l[k]) * buses.dry_l[k] + double(buses.dry_r[k]) * buses.dry_r[k];
        }
        return energy;
    }
};

double noise(std::size_t i) { return std::sin(double(i) * 0.37) * std::sin(double(i) * 0.0123); }

} // namespace

TEST_CASE("fx_input_gain_ramps") {
    // syxg50: 4/128 per 10 ms tick (−2.0 after ≈ 0.64 s); 2006LE: 10/128 per ≈ 12 ms.
    Rig s50(true);
    CHECK(s50.sys().reverb_input_gain() == 0.0f);
    s50.run(0.4f);
    CHECK(s50.sys().reverb_input_gain() > 1.0f);
    CHECK(s50.sys().reverb_input_gain() < 2.0f);
    s50.run(0.4f);
    CHECK(s50.sys().reverb_input_gain() == 2.0f);
    CHECK(s50.sys().chorus_input_gain() < 3.21875f);
    s50.run(0.4f);
    CHECK(s50.sys().chorus_input_gain() == 3.21875f);

    Rig le(false);
    le.run(0.35f);
    CHECK(le.sys().reverb_input_gain() == 2.0f);
    le.run(0.3f);
    CHECK(le.sys().chorus_input_gain() == 3.21875f);
    // A type change restarts the ramp.
    le.set(0x20, 0x43);
    le.run(0.05f);
    CHECK(le.sys().chorus_input_gain() < 1.0f);
}

TEST_CASE("fx_chorus_return_law") {
    HVYXG_NEED_TABLES();
    for (const bool syxg50 : {true, false}) {
        Rig rig(syxg50);
        rig.run(1.2f);
        const auto feed = [](Buses& b, std::size_t t) {
            for (std::size_t k = 0; k < Buses::kMaxBlock; ++k)
                b.chorus_l[k] = b.chorus_r[k] = static_cast<float>(0.1 * noise(t + k));
        };
        const double e64 = rig.run(0.5f, feed);
        rig.set(0x2C, 127);
        const double e127 = rig.run(0.5f, feed);
        // Return words 182 → 362 (512·2^(−a/16) at centre pan).
        // (The LFO moves on between the two halves, hence the tolerance.)
        CHECK(std::sqrt(e127 / e64) == doctest::Approx(362.0 / 182.0).epsilon(0.04));
    }
}

TEST_CASE("fx_unsupported_types_are_off") {
    for (const bool syxg50 : {true, false}) {
        Rig rig(syxg50);
        rig.set(0x00, 0x12);  // Canyon
        rig.set(0x20, 0x44);  // Symphonic as system chorus
        rig.run(1.2f);
        const double e = rig.run(0.5f, [](Buses& b, std::size_t t) {
            for (std::size_t k = 0; k < Buses::kMaxBlock; ++k)
                b.reverb[k] = b.chorus_l[k] = b.chorus_r[k] = static_cast<float>(noise(t + k));
        });
        CHECK(e == 0.0);
        CHECK_FALSE(rig.fx->active());
    }
}

TEST_CASE("fx_reverb_tail_and_sleep") {
    HVYXG_NEED_TABLES();
    Rig rig(false);
    rig.run(1.0f);
    CHECK_FALSE(rig.fx->active());
    const double wet = rig.run(0.1f, [](Buses& b, std::size_t t) {
        if (t == 0) b.reverb[0] = 0.5f;
    });
    CHECK(wet > 0.0);
    CHECK(rig.fx->active());
    rig.run(8.0f);  // Hall 1 decays far below the bus unit, then the gate closes
    CHECK_FALSE(rig.fx->active());
}

TEST_CASE("fx_ram_type_change_reloads_defaults") {
    HVYXG_NEED_TABLES();
    midi::xg::EffectData ram{test::tables().get()};
    ram.reset();
    CHECK(ram[0x12] == 52);  // ER / rev balance of the XG reset image
    ram.write(0x00, 0x10);  // White Room
    CHECK(ram[0x02] == 9);
    CHECK(ram[0x07] == 30);
    CHECK(ram[0x0A] == 7);
    CHECK(ram[0x10] == 34);
    ram.write(0x20, 0x41);
    ram.write(0x21, 0x08);  // Chorus 4
    CHECK(ram[0x22] == 9);
    CHECK(ram[0x25] == 104);
    CHECK(ram[0x34] == 1);  // stereo input
}

TEST_CASE("fx_type_resend_and_reset_reload_the_units") {
    HVYXG_NEED_TABLES();
    for (const bool syxg50 : {true, false}) {
        Rig rig(syxg50);
        rig.run(1.5f);
        REQUIRE(rig.sys().reverb_input_gain() == 2.0f);
        REQUIRE(rig.sys().chorus_input_gain() == 3.21875f);
        // Ring the reverb, then re-send the same type (Hall 1): the tail is cleared and the
        // input ramp restarts.
        rig.run(0.05f, [](Buses& b, std::size_t t) {
            if (t == 0) b.reverb[0] = 0.5f;
        });
        REQUIRE(rig.fx->active());
        rig.set(0x00, 0x01);
        CHECK(rig.run(0.2f) == 0.0);
        CHECK(rig.sys().reverb_input_gain() < 2.0f);
        CHECK(rig.sys().chorus_input_gain() == 3.21875f);
        rig.set(0x20, 0x41);  // same chorus type
        rig.run(0.01f);
        CHECK(rig.sys().chorus_input_gain() < 1.0f);

        // A reset (XG / GM System On reset the effect RAM) restarts both ramps.
        rig.run(1.5f);
        REQUIRE(rig.sys().reverb_input_gain() == 2.0f);
        rig.ram.reset();
        rig.run(0.01f);
        CHECK(rig.sys().reverb_input_gain() < 1.0f);
        CHECK(rig.sys().chorus_input_gain() < 1.0f);
    }
}
