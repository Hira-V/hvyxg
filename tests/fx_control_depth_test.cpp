// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Effect control depths: the variation's (02 01 5C..60, both DLLs) and the XG2.0 insertion
// blocks' (03 nn 0D..11, superset, MU2000 behaviour). Expected values were measured:
// syxg50.dll by rendering (Process=1) and comparing at lag 0, S-YXG2006LE.DLL from its
// effect control parameter words (dumps after each controller event), the MU2000 with
// S-MU2000 (a control render against renders with the parameter written instead).
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "fx/fx_system.hpp"
#include "midi/part.hpp"
#include "midi/xg_ram.hpp"
#include "syxg2006le/effects.hpp"
#include "syxg50/effects.hpp"
#include "test_support.hpp"

using namespace hvyxg;
using fx::ControlLaw;

namespace {

struct Rig {
    midi::xg::MultiPart mp{0};
    midi::Part part;
    midi::xg::EffectData eff{test::tables().get()};
    midi::xg::EffectInsertions ins = test::insertions();
    midi::xg::System sys;
    std::unique_ptr<SystemEffects> fx;
    std::size_t block;
    Buses buses;

    explicit Rig(bool syxg50) : block(syxg50 ? 128 : 64) {
        part.ram = &mp;
        eff.reset();
        ModuleContext ctx;
        ctx.tables = test::tables().get();
        ctx.shared.effect1 = &eff;
        ctx.shared.effect_insertion = &ins;
        ctx.shared.system = &sys;
        ctx.shared.parts = &part;
        ctx.shared.part_count = 1;
        ctx.sample_rate = 44100.0f;
        fx = syxg50 ? syxg50::make_system_effects(ctx) : syxg2006le::make_system_effects(ctx);
    }
    const fx::SystemFx& sys_fx() const { return dynamic_cast<const fx::SystemFx&>(*fx); }
    std::uint16_t var(std::uint8_t i) const { return sys_fx().control().parameter(fx::Block::Variation, i); }
    std::uint16_t ins_param(std::size_t i, std::size_t blk = 0) const {
        return sys_fx().insertion().applied_parameter(blk, i).value_or(0xFFFF);
    }
    /// Run blocks; `chain` feeds a little signal into the insertion chain of part 0.
    std::vector<float> run(float seconds) {
        std::vector<float> out;
        const auto n = static_cast<std::size_t>(seconds * 44100.0f);
        for (std::size_t t = 0; t < n; t += block) {
            buses.clear();
            fx->prepare(buses);
            if (buses.chain_of(0) == 0)
                for (std::size_t k = 0; k < block; ++k)
                    buses.chain_l[0][k] = buses.chain_r[0][k] = 0.25f * static_cast<float>(((t + k) * 7919u) % 64u) / 64.0f - 0.125f;
            fx->process(buses, block);
            out.insert(out.end(), buses.dry_l.begin(), buses.dry_l.begin() + static_cast<std::ptrdiff_t>(block));
        }
        return out;
    }
    void insertion_variation(std::uint8_t msb, std::uint8_t depth_addr, std::uint8_t depth) {
        eff.write(0x40, msb);
        eff.write(0x41, 0);
        eff.write(0x5A, 0);
        eff.write(0x5B, 0);
        eff.write(depth_addr, depth);
    }
};

std::array<std::uint8_t, 5> depths(std::size_t src, std::uint8_t d) {
    std::array<std::uint8_t, 5> a = {64, 64, 64, 64, 64};
    a[src] = d;
    return a;
}

} // namespace

TEST_CASE("fx_control_law") {
    midi::xg::MultiPart mp(0);
    midi::Part p;
    p.ram = &mp;
    // 2006LE and MU2000: MW at depth 127 (+63): trunc(v · 63 / 64).
    const std::array<int, 12> v = {0, 1, 2, 31, 32, 33, 63, 64, 65, 100, 126, 127};
    const std::array<int, 12> up = {0, 0, 1, 30, 31, 32, 62, 63, 63, 98, 124, 125};
    const std::array<int, 12> down = {0, -1, -2, -31, -32, -33, -63, -64, -65, -100, -126, -127};
    for (std::size_t k = 0; k < v.size(); ++k) {
        p.controller.modulation = static_cast<std::uint8_t>(v[k]);
        CHECK(fx::control_offset(depths(0, 127).data(), p, ControlLaw::Centred) == up[k]);
        CHECK(fx::control_offset(depths(0, 0).data(), p, ControlLaw::Centred) == down[k]);
        CHECK(fx::control_offset(depths(0, 127).data(), p, ControlLaw::Syxg50) == up[k]);
    }
    p.controller.modulation = 0;
    // Bend, 2006LE / MU2000: (bend − 8192) · d / 8192 rounded toward zero.
    const auto bend = [&](std::uint16_t b, std::uint8_t d, ControlLaw law) {
        p.pitchbend = b;
        return fx::control_offset(depths(1, d).data(), p, law);
    };
    CHECK(bend(16383, 127, ControlLaw::Centred) == 62);
    CHECK(bend(8192, 127, ControlLaw::Centred) == 0);
    CHECK(bend(8256, 127, ControlLaw::Centred) == 0);
    CHECK(bend(4096, 127, ControlLaw::Centred) == -31);
    CHECK(bend(200, 100, ControlLaw::Centred) == -35);
    CHECK(bend(0, 65, ControlLaw::Centred) == -1);
    CHECK(bend(16, 65, ControlLaw::Centred) == 0);
    CHECK(bend(8064, 0, ControlLaw::Centred) == 1);
    CHECK(bend(8128, 0, ControlLaw::Centred) == 0);
    CHECK(bend(0, 0, ControlLaw::Centred) == 64);
    // syxg50: (bend − 32) · d / 32 on the 14-bit bend, so the centre already saturates.
    CHECK(bend(8192, 127, ControlLaw::Syxg50) == 127);
    CHECK(bend(8192, 0, ControlLaw::Syxg50) == -127);
    CHECK(bend(32, 127, ControlLaw::Syxg50) == 0);
    CHECK(bend(0, 0, ControlLaw::Syxg50) == 64);
    // The sum wraps at 16 bits before the clip: (16383 − 32) · −64 / 32 = −32702, plus
    // CAT −127 → −32829 → +32707 → +127.
    p.pitchbend = 16383;
    p.cat_value = 127;
    std::array<std::uint8_t, 5> both = {64, 0, 0, 64, 64};
    CHECK(fx::control_offset(both.data(), p, ControlLaw::Syxg50) == 127);
    CHECK(fx::control_offset(both.data(), p, ControlLaw::Centred) == -127);
    // CAT / AC1 / AC2 use the same law as MW; AC1 / AC2 read their assigned CC.
    p.pitchbend = 8192;
    p.cat_value = 64;
    CHECK(fx::control_offset(depths(2, 127).data(), p, ControlLaw::Centred) == 63);
    p.controller.ac[0] = 64;
    p.controller.ac[1] = 64;
    CHECK(fx::control_offset(depths(3, 127).data(), p, ControlLaw::Centred) == 63);
    CHECK(fx::control_offset(depths(4, 100).data(), p, ControlLaw::Centred) == 36);
    // AC1 +63 and CAT −64 (MU2000: 40 → 39).
    std::array<std::uint8_t, 5> sum = {64, 64, 0, 127, 64};
    CHECK(fx::control_offset(sum.data(), p, ControlLaw::Centred) == -1);

    // Controlled parameter (syxg50 table; MU2000 for the XG2.0 types).
    CHECK(fx::controlled_parameter(0x00) == -1);
    CHECK(fx::controlled_parameter(0x49) == 0);
    CHECK(fx::controlled_parameter(0x45) == 0);
    CHECK(fx::controlled_parameter(0x48) == 9);
    CHECK(fx::controlled_parameter(0x4E) == 2);
    CHECK(fx::controlled_parameter(0x41) == 9);
    CHECK(fx::controlled_parameter(0x53) == -1);
    CHECK(fx::controlled_parameter(0x54) == -1);
    CHECK(fx::controlled_parameter(0x71) == 0);
    CHECK(fx::controlled_parameter(0x5E) == 9);
    // Dry/wet (1..127) moves by one less than the offset (2006LE and MU2000).
    CHECK(fx::scale_control(9, 1, 127) == 8);
    CHECK(fx::scale_control(-64, 1, 127) == -63);
    CHECK(fx::scale_control(-1, 1, 127) == 0);
    CHECK(fx::scale_control(-33, 0, 127) == -33);
}

TEST_CASE("fx_control_variation_syxg50_bend_centre") {
    HVYXG_NEED_TABLES();
    // syxg50.dll: with a positive bend depth a centred bend already drives the controlled
    // parameter to its limit, 4 units per 10-ms tick (distortion drive 40 → 127).
    Rig r(true);
    r.insertion_variation(0x49, 0x5D, 127);
    r.run(0.05f);
    const auto early = r.var(0);
    CHECK(early > 40);
    CHECK(early < 127);
    r.run(0.4f);
    CHECK(r.var(0) == 127);
    // Bend depth 64: no offset, the RAM value comes back by 4 per tick.
    r.eff.write(0x5D, 64);
    r.run(0.4f);
    CHECK(r.var(0) == 40);
}

TEST_CASE("fx_control_variation_syxg50_slew") {
    HVYXG_NEED_TABLES();
    // MW 64 at depth 127: +63, reached 4 per tick (16 ticks ≈ 0.16 s), never at once.
    Rig r(true);
    r.insertion_variation(0x49, 0x5C, 127);
    r.run(0.2f);
    r.part.controller.modulation = 64;
    r.run(0.003f);
    CHECK(r.var(0) <= 44);
    r.run(0.3f);
    CHECK(r.var(0) == 103);
}

TEST_CASE("fx_control_variation_syxg50_out_of_range") {
    HVYXG_NEED_TABLES();
    // syxg50.dll (Process=1 renders of flanger, symphonic and chorus in insertion connection,
    // matched to −90 … −122 dB): the value sent is the RAM low byte + offset clipped to
    // 0..127, and the effect control rejects one outside the parameter's range. Dry/wet 64 at AC1
    // depth 0 with AC1 127 steps 60, 56, …, 4, then 0 is rejected: dry/wet stays at 4.
    for (const std::uint8_t msb : {std::uint8_t{0x43}, std::uint8_t{0x44}, std::uint8_t{0x41}}) {
        CAPTURE(int(msb));
        Rig r(true);
        r.insertion_variation(msb, 0x5F, 0);
        r.eff.write(0x54, 0);
        r.eff.write(0x55, 64);
        r.run(0.05f);
        CHECK(r.var(9) == 64);
        r.part.controller.ac[0] = 127;
        r.run(0.5f);
        CHECK(r.var(9) == 4);
        r.part.controller.ac[0] = 0;
        r.run(0.5f);
        CHECK(r.var(9) == 64);
    }
}

TEST_CASE("fx_control_variation_2006le") {
    HVYXG_NEED_TABLES();
    // S-YXG2006LE.DLL control words: the offset applies in the block of the
    // controller event, without a slew.
    Rig r(false);
    r.insertion_variation(0x49, 0x5C, 127);
    r.run(0.1f);
    CHECK(r.var(0) == 40);
    r.part.controller.modulation = 32;
    r.run(0.0015f);
    CHECK(r.var(0) == 71);
    r.part.controller.modulation = 0;
    r.run(0.0015f);
    CHECK(r.var(0) == 40);
    // Depth 0 (−64): 40 − v.
    r.eff.write(0x5C, 0);
    r.part.controller.modulation = 33;
    r.run(0.0015f);
    CHECK(r.var(0) == 7);
    // A parameter write replaces the offset value until the offset changes; a type change
    // re-applies it (overdrive drive 29 + 29).
    r.eff.write(0x5C, 127);
    r.part.controller.modulation = 64;
    r.run(0.0015f);
    CHECK(r.var(0) == 103);
    r.eff.write(0x42, 0);
    r.eff.write(0x43, 50);
    r.run(0.0015f);
    CHECK(r.var(0) == 50);
    r.part.controller.modulation = 65;  // still +63
    r.run(0.0015f);
    CHECK(r.var(0) == 50);
    r.part.controller.modulation = 30;
    r.run(0.0015f);
    CHECK(r.var(0) == 79);
    r.eff.write(0x40, 0x4A);
    r.run(0.0015f);
    CHECK(r.var(0) == 58);
    // Bend: centred law (16383 at +63 → +62).
    r.part.controller.modulation = 0;
    r.eff.write(0x5C, 64);
    r.eff.write(0x5D, 127);
    r.part.pitchbend = 16383;
    r.run(0.0015f);
    CHECK(r.var(0) == 29 + 62);
}

TEST_CASE("fx_control_variation_2006le_dry_wet") {
    HVYXG_NEED_TABLES();
    // Chorus dry/wet 64: depth 127 v = 1, 2, 10, 33, 64, 127 → 64 64 72 95 126 127;
    // depth 0 → 64 63 55 32 1 1 (the offset is scaled to the 1..127 range).
    const std::array<std::uint8_t, 6> v = {1, 2, 10, 33, 64, 127};
    const std::array<std::uint16_t, 6> up = {64, 64, 72, 95, 126, 127};
    const std::array<std::uint16_t, 6> down = {64, 63, 55, 32, 1, 1};
    for (const std::uint8_t d : {std::uint8_t{127}, std::uint8_t{0}}) {
        Rig r(false);
        r.insertion_variation(0x41, 0x5C, d);
        r.run(0.05f);
        for (std::size_t k = 0; k < v.size(); ++k) {
            r.part.controller.modulation = v[k];
            r.run(0.0015f);
            CHECK(r.var(9) == (d == 127 ? up[k] : down[k]));
        }
    }
}

namespace {

void insertion_block(Rig& r, std::uint8_t msb, std::initializer_list<std::pair<std::uint8_t, std::uint8_t>> writes) {
    r.ins[0].write(0x00, msb);
    r.ins[0].write(0x01, 0);
    r.ins[0].write(0x0C, 0);
    for (const auto& [a, v] : writes) r.ins[0].write(a, v);
}

} // namespace

TEST_CASE("fx_control_insertion_off_is_unchanged") {
    HVYXG_NEED_TABLES();
    // Depths at 64: controllers change nothing, bit for bit; a depth with its source at 0
    // changes nothing either.
    for (const bool syxg50 : {true, false}) {
        CAPTURE(syxg50);
        Rig a(syxg50), b(syxg50), c(syxg50);
        insertion_block(a, 0x49, {});
        insertion_block(b, 0x49, {});
        insertion_block(c, 0x49, {{0x10, 127}, {0x0F, 0}});
        b.part.controller.modulation = 100;
        b.part.cat_value = 90;
        b.part.pitchbend = 300;
        b.part.controller.ac[0] = 70;
        const std::vector<float> ya = a.run(0.3f);
        double e = 0;
        for (float x : ya) e += double(x) * x;
        CHECK(e > 1.0);  // the chain sounds
        CHECK(ya == b.run(0.3f));
        CHECK(ya == c.run(0.3f));
        CHECK(a.ins_param(0) == 40);
        CHECK(c.ins_param(0) == 40);
    }
}

TEST_CASE("fx_control_insertion_sources") {
    HVYXG_NEED_TABLES();
    // MU2000 (S-MU2000): each source drives the type's controlled parameter at once, with the
    // centred law; the parameter is the RAM value plus the offset.
    struct Case {
        std::uint8_t msb;
        std::uint8_t base_addr, base;
        std::uint8_t depth_addr, depth;
        int src;  // 0 MW, 1 bend, 2 CAT, 3 AC1, 4 AC2
        std::uint16_t value;
        std::size_t param;
        std::uint16_t expect;
    };
    const Case cases[] = {
        {0x49, 0x02, 40, 0x0D, 127, 0, 64, 0, 103},     // MW → drive
        {0x49, 0x02, 40, 0x0E, 127, 1, 16383, 0, 102},  // bend max
        {0x49, 0x02, 64, 0x0E, 127, 1, 4096, 0, 33},
        {0x49, 0x02, 64, 0x0E, 100, 1, 200, 0, 29},
        {0x49, 0x02, 40, 0x0F, 127, 2, 64, 0, 103},     // CAT
        {0x49, 0x02, 40, 0x10, 127, 3, 2, 0, 41},       // AC1
        {0x49, 0x02, 40, 0x10, 127, 3, 65, 0, 103},
        {0x49, 0x02, 100, 0x10, 0, 3, 33, 0, 67},
        {0x49, 0x02, 100, 0x10, 0, 3, 64, 0, 36},
        {0x49, 0x02, 127, 0x10, 0, 3, 127, 0, 0},
        {0x49, 0x02, 40, 0x11, 100, 4, 64, 0, 76},      // AC2
        {0x45, 0x02, 100, 0x10, 0, 3, 33, 0, 67},       // rotary speed
        {0x4E, 0x04, 100, 0x10, 0, 3, 33, 2, 67},       // auto wah cutoff
        {0x41, 0x0B, 100, 0x10, 0, 3, 33, 9, 68},       // chorus dry/wet (1..127)
        {0x41, 0x0B, 2, 0x10, 127, 3, 34, 9, 34},
        {0x41, 0x0B, 50, 0x10, 0, 3, 20, 9, 31},
        {0x5E, 0x0B, 100, 0x10, 0, 3, 33, 9, 68},       // Lo-Fi dry/wet
        {0x5E, 0x0B, 50, 0x10, 127, 3, 16, 9, 64},
        {0x71, 0x02, 60, 0x10, 0, 3, 32, 0, 28},        // ring modulator frequency
    };
    for (const auto& k : cases) {
        CAPTURE(int(k.msb));
        CAPTURE(int(k.depth_addr));
        CAPTURE(k.value);
        Rig r(false);
        insertion_block(r, k.msb, {{k.base_addr, k.base}, {k.depth_addr, k.depth}});
        r.run(0.02f);
        switch (k.src) {
        case 0: r.part.controller.modulation = static_cast<std::uint8_t>(k.value); break;
        case 1: r.part.pitchbend = k.value; break;
        case 2: r.part.cat_value = static_cast<std::uint8_t>(k.value); break;
        case 3: r.part.controller.ac[0] = static_cast<std::uint8_t>(k.value); break;
        default: r.part.controller.ac[1] = static_cast<std::uint8_t>(k.value); break;
        }
        r.run(0.0015f);
        CHECK(r.ins_param(k.param) == k.expect);
    }
}

TEST_CASE("fx_control_insertion_flanger_symphonic_eq") {
    HVYXG_NEED_TABLES();
    // MU2000 (S-MU2000, a control render against renders with dry/wet written instead,
    // bit-exact at the measured value and −34 … −41 dB one step away): the flanger and the
    // symphonic take the control on dry/wet with the chorus law, base + offset · 126 / 127
    // rounded toward zero; the 3-band and 2-band EQs have no controlled parameter.
    struct Case {
        std::uint8_t msb, lsb, base, depth, ac1;
        std::uint16_t expect;
    };
    const Case cases[] = {
        {0x43, 0, 40, 127, 64, 102}, {0x43, 0, 100, 0, 33, 68}, {0x43, 8, 20, 100, 90, 69},
        {0x44, 0, 40, 127, 64, 102}, {0x44, 0, 100, 0, 33, 68}, {0x44, 0, 90, 30, 120, 28},
        {0x41, 0, 40, 127, 64, 102}, {0x41, 0, 100, 0, 33, 68},
    };
    for (const bool syxg50 : {true, false}) {
        for (const auto& k : cases) {
            CAPTURE(syxg50);
            CAPTURE(int(k.msb));
            CAPTURE(int(k.base));
            Rig r(syxg50);
            insertion_block(r, k.msb, {{0x01, k.lsb}, {0x0B, k.base}, {0x10, k.depth}});
            r.run(0.02f);
            CHECK(r.ins_param(9) == k.base);
            r.part.controller.ac[0] = k.ac1;
            r.run(0.0015f);
            CHECK(r.ins_param(9) == k.expect);
        }
        for (const std::uint8_t msb : {std::uint8_t{0x4C}, std::uint8_t{0x4D}}) {
            Rig a(syxg50), b(syxg50);
            insertion_block(a, msb, {{0x10, 127}});
            insertion_block(b, msb, {{0x10, 127}});
            a.run(0.02f);
            b.run(0.02f);
            b.part.controller.ac[0] = 127;
            CHECK(a.run(0.1f) == b.run(0.1f));
        }
    }
}

TEST_CASE("fx_control_insertion_keeps_offset") {
    HVYXG_NEED_TABLES();
    // MU2000: the offset stays on the parameter through a parameter write (20 + 63) and a
    // type change (overdrive, drive 30 + 31); the compressor has no controlled parameter.
    Rig r(true);
    insertion_block(r, 0x49, {{0x02, 40}, {0x10, 127}});
    r.part.controller.ac[0] = 64;
    r.run(0.01f);
    CHECK(r.ins_param(0) == 103);
    r.ins[0].write(0x02, 20);
    r.run(0.01f);
    CHECK(r.ins_param(0) == 83);
    r.part.controller.ac[0] = 32;
    r.ins[0].write(0x00, 0x4A);
    r.ins[0].write(0x02, 30);
    r.run(0.01f);
    CHECK(r.ins_param(0) == 61);
    insertion_block(r, 0x53, {{0x02, 40}});
    r.run(0.01f);
    CHECK(r.ins_param(0) == 40);
}

TEST_CASE("fx_control_cbc_depths_do_nothing") {
    // CBC1 / CBC2 (insertion 03 nn 12 / 13, variation 02 01 61 / 62): the MU2000 has no such
    // sources. S-MU2000, distortion insertion block and insertion-connection variation on an
    // organ chord, every CC 1..95 (bar the volume / pan / pedal / effect-send ones), CAT and
    // bend swept to their maxima and back: depths 00 / 7F in either slot render bit-identical to
    // 40 / 40, while an AC1 depth of 7F in the same probe changes the output (−22 dB). Neither
    // DLL reads them either.
    for (const bool syxg50 : {true, false}) {
        CAPTURE(syxg50);
        Rig a(syxg50), b(syxg50);
        insertion_block(a, 0x49, {});
        insertion_block(b, 0x49, {{0x12, 0x7F}, {0x13, 0x00}});
        a.insertion_variation(0x49, 0x61, 0x40);
        b.insertion_variation(0x49, 0x61, 0x7F);
        b.eff.write(0x62, 0x00);
        for (Rig* r : {&a, &b}) {
            r->part.controller.modulation = 127;
            r->part.cat_value = 127;
            r->part.pitchbend = 16383;
            r->part.controller.ac[0] = 127;
            r->part.controller.ac[1] = 127;
        }
        CHECK(a.run(0.3f) == b.run(0.3f));
    }
}
