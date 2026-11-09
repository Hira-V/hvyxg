// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Variation block: type map, control words, a kernel impulse response and the system-effects
// mixer (return, sends, ramps), with values of syxg50.dll / S-YXG2006LE.DLL (register dumps
// and kernel runs). The voice-side routing is checked with the real banks (skipped when the
// bank variables are unset).
#include <doctest/doctest.h>

#define XXH_INLINE_ALL
#include "xxhash/xxhash.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "hvyxg/bank.hpp"
#include "fx/fx_control.hpp"
#include "fx/fx_system.hpp"
#include "fx/fx_tables.hpp"
#include "fx/fx_variation.hpp"
#include "renderer.hpp"
#include "syxg2006le/effects.hpp"
#include "syxg50/effects.hpp"
#include "midi/part.hpp"
#include "midi/xg_ram.hpp"
#include "test_support.hpp"

using namespace hvyxg;
using fx::Block;
using fx::VariationType;

namespace {

struct FxRig {
    midi::xg::EffectData ram{test::tables().get()};
    std::unique_ptr<SystemEffects> fx;
    std::size_t block;
    Buses buses;

    explicit FxRig(bool syxg50) : block(syxg50 ? 128 : 64) {
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
    /// Run `frames`; `feed` fills the variation buses of each block (frame offset given).
    std::vector<float> run(std::size_t frames, const std::function<void(Buses&, std::size_t)>& feed = {}) {
        std::vector<float> left;
        for (std::size_t t = 0; t < frames; t += block) {
            buses.clear();
            if (feed) feed(buses, t);
            fx->process(buses, block);
            left.insert(left.end(), buses.dry_l.begin(), buses.dry_l.begin() + static_cast<long>(block));
        }
        return left;
    }
};

} // namespace

TEST_CASE("fx_variation_type_map") {
    HVYXG_NEED_TABLES();
    using fx::variation_type;
    CHECK(variation_type(test::fx_tables(), 0x05, 0, false) == VariationType{0, 14});  // Delay L,C,R
    CHECK(variation_type(test::fx_tables(), 0x01, 1, true) == VariationType{3, 2});    // Hall 2
    CHECK(variation_type(test::fx_tables(), 0x09, 1, true) == VariationType{2, 2});    // Early Ref 2
    CHECK(variation_type(test::fx_tables(), 0x14, 2, true) == VariationType{3, 12});   // Karaoke 3
    CHECK(variation_type(test::fx_tables(), 0x41, 8, true) == VariationType{0, 20});   // Chorus 4
    CHECK(variation_type(test::fx_tables(), 0x48, 8, true) == VariationType{1, 2});    // Phaser 2
    CHECK(variation_type(test::fx_tables(), 0x4E, 3, true) == VariationType{3, 9});    // Auto Wah (any LSB)
    CHECK(variation_type(test::fx_tables(), 0x00, 0, true) == VariationType{0, 0});    // No Effect: off
    // Unknown MSBs: off in system connection, Thru in insertion; above 0x4E always Thru.
    CHECK(variation_type(test::fx_tables(), 0x30, 0, true) == VariationType{0, 0});
    CHECK(variation_type(test::fx_tables(), 0x30, 0, false) == VariationType{0, 1});
    CHECK(variation_type(test::fx_tables(), 0x60, 0, true) == VariationType{0, 1});
    // A type write reloads the type's defaults into the RAM (Delay L,R: 2500, 3750, 3750, 3750).
    midi::xg::EffectData ram{test::tables().get()};
    ram.reset();
    ram.write(0x40, 0x06);
    CHECK(ram.variation_param(0) == fx::variation_defaults(test::fx_tables(), {0, 15})[0]);
    CHECK(fx::variation_defaults(test::fx_tables(), {0, 14})[0] == 3333);
}

TEST_CASE("fx_xg_reset_image_hash") {
    HVYXG_NEED_TABLES();
    // The composed reset image (EffectData::reset) is the S-YXG2006LE's XG effect RAM image byte
    // for byte: its xxHash3-128 is that image's.
    midi::xg::EffectData ram{test::tables().get()};
    ram.reset();
    std::array<std::uint8_t, 128> b{};
    for (std::size_t i = 0; i < b.size(); ++i) b[i] = ram[static_cast<std::uint8_t>(i)];
    const XXH128_hash_t h = XXH3_128bits(b.data(), b.size());
    CHECK(h.high64 == 0x0790c38e3e73a3f1ull);
    CHECK(h.low64 == 0xb256e7e50cc20ac9ull);
}

TEST_CASE("fx_variation_xg_reset_image") {
    HVYXG_NEED_TABLES();
    // Both DLLs reset to Delay L,C,R with C level 127, insertion connection, part off.
    midi::xg::EffectData ram{test::tables().get()};
    ram.reset();
    CHECK(ram[0x40] == 0x05);
    CHECK(ram.variation_param(0) == 3333);
    CHECK(ram.variation_param(5) == 127);
    CHECK(ram.variation_param(9) == 32);
    CHECK(ram.variation_connection() == 0);
    CHECK(ram.variation_part() == 0x7F);
    CHECK(ram.variation_param(11) == 60);
}

TEST_CASE("fx_variation_delay_lcr_control_and_kernel") {
    HVYXG_NEED_TABLES();
    // XG reset image, insertion connection: the DLLs' register dumps.
    fx::EffectControl ctrl(test::fx_tables());
    ctrl.set_variation_type({0, 14});
    ctrl.set_parameter(Block::Variation, 5, 127);
    CHECK(ctrl.addr(0x3a) == 1);       // line 1
    CHECK(ctrl.addr(0x33) == 14699);   // L 333.3 ms
    CHECK(ctrl.addr(0x2d) == 7352);    // R 166.7 ms
    CHECK(ctrl.addr(0x38) == 22050);   // C 500 ms
    CHECK(ctrl.addr(0x28) == 22050);   // feedback 500 ms
    CHECK(ctrl.word(0xab) == 78);      // feedback 74 → (74 − 64)·125 >> 4
    CHECK(ctrl.word(0xb2) == 512);     // dry (dry/wet 32)
    CHECK(ctrl.word(0xb5) == 62);      // wet, halved in insertion
    CHECK(ctrl.word(0xb6) == 62);      // C level 127 · wet
    // Impulse into L: R tap at A2d − A3a, L tap at A33 − A3a, C on both at A38 − A3a.
    fx::VariationUnit unit(44100.0f);
    unit.load(ctrl);
    std::vector<float> l(64), r(64), out_l, out_r;
    for (int b = 0; b < 400; ++b) {
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        if (b == 0) l[0] = 1.0f;
        unit.process(l.data(), r.data(), 64);
        out_l.insert(out_l.end(), l.begin(), l.end());
        out_r.insert(out_r.end(), r.begin(), r.end());
    }
    CHECK(out_r[7351] == doctest::Approx(62.0 / 512).epsilon(1e-5));
    CHECK(out_l[14698] == doctest::Approx(62.0 / 512).epsilon(1e-5));
    CHECK(out_l[22049] == doctest::Approx(62.0 / 512).epsilon(1e-5));
    CHECK(out_r[22049] == doctest::Approx(62.0 / 512).epsilon(1e-5));
    CHECK(std::fabs(out_r[7350]) < 1e-6f);
}

TEST_CASE("fx_variation_system_mixer") {
    HVYXG_NEED_TABLES();
    // return 127 at centre = 362/512, variation → reverb at 127 = 1.0,
    // → chorus at 127 = 73/128 (mono chorus), input gain −2.83 (/128 362) after its ramp.
    FxRig rig(true);
    rig.set(0x5A, 1);
    rig.set(0x56, 127);
    rig.set(0x58, 127);
    rig.set(0x59, 127);
    rig.run(44100 * 3 / 2);
    CHECK(rig.sys().variation_input_gain() == doctest::Approx(362.0 / 128));
    CHECK(rig.sys().variation_return(false) == doctest::Approx(362.0 / 512));
    CHECK(rig.sys().variation_return(true) == doctest::Approx(362.0 / 512));
    CHECK(rig.sys().variation_to_reverb(false) == doctest::Approx(test::fx_tables().kMixerWord[0x18 + 8] / 128.0));
    CHECK(rig.sys().variation_to_chorus(false) == doctest::Approx(73.0 / 128));
    // A flanger as chorus (stereo input) takes the send with 0x15 instead of 0x25: the chorus
    // type change restarts the send from 0 and recomputes its target.
    rig.set(0x20, 0x43);
    rig.set(0x21, 0);
    rig.run(rig.block);
    CHECK(rig.sys().variation_to_chorus(false) < 0.1f);
    rig.run(44100);
    CHECK(rig.sys().variation_to_chorus(false) ==
          doctest::Approx(test::fx_tables().kMixerWord[0x15 + 8] / 128.0));

    // Thru: an impulse returns × input gain × return (the unit kept awake so that its
    // output fade is up).
    rig.set(0x40, 0x40);
    rig.set(0x41, 0);
    rig.run(44100 * 2, [](Buses& b, std::size_t) { b.variation_r[0] = 1e-3f; });
    const auto out = rig.run(256, [](Buses& b, std::size_t t) {
        if (t == 128) b.variation_l[5] = 0.1f;
    });
    CHECK(out[128 + 5] == doctest::Approx(0.1 * 362.0 / 128 * 362.0 / 512).epsilon(1e-4));

    // Insertion connection with no part: nothing returns.
    rig.set(0x5A, 0);
    rig.run(44100);
    CHECK(rig.sys().variation_return(false) == 0.0f);
    CHECK(rig.sys().variation_input_gain() == doctest::Approx(1.0));
}

TEST_CASE("fx_variation_system_return_restarts_after_a_reset") {
    HVYXG_NEED_TABLES();
    // (every reset sets the units up again): a system-connection return
    // and its sends slew up from 0 by 8 per tick. Seen after a GM2 On, which keeps the effect
    // RAM [meas: Process=1, system Chorus with return 127 set before GM2 On, −110 dB].
    FxRig rig(true);
    rig.set(0x5A, 1);
    rig.set(0x56, 127);
    rig.run(44100);
    CHECK(rig.sys().variation_return(false) == doctest::Approx(362.0 / 512));
    rig.ram.restart();
    rig.run(rig.block);
    CHECK(rig.sys().variation_return(false) < 0.1f);
    rig.run(44100);
    CHECK(rig.sys().variation_return(false) == doctest::Approx(362.0 / 512));
}

TEST_CASE("fx_variation_running_unit_keeps_its_lfo_start") {
    HVYXG_NEED_TABLES();
    // A type set while the unit runs keeps it running (the LFO starts at the change, not at
    // the next input; measured on syxg50): two runs that differ only in the idle time
    // between the type change and the input give the same output when the unit was running.
    const auto play = [](std::size_t idle) {
        FxRig rig(true);
        rig.set(0x5A, 1);
        rig.set(0x56, 127);
        rig.run(44100);
        // Keep the unit awake with a quiet input, then switch to Chorus 1.
        rig.run(4096, [](Buses& b, std::size_t) { b.variation_l[0] = 1e-3f; });
        rig.set(0x40, 0x41);
        rig.set(0x41, 0);
        rig.run(idle);
        return rig.run(4096, [](Buses& b, std::size_t t) { b.variation_l[0] = t == 0 ? 0.5f : 0.0f; });
    };
    const auto a = play(0), b = play(1024);
    double diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff += std::fabs(a[i] - b[i]);
    CHECK(diff > 1e-3);  // the LFO moved on during the idle time
}

namespace {

struct SongRig : test::Rig {
    explicit SongRig(bool syxg50) : test::Rig(test::bank(syxg50), 64) {
        // No reverb, so that only the routing under test differs.
        cc(0, 91, 0);
    }
    void xg(std::uint8_t addr, std::uint8_t a, std::optional<std::uint8_t> b = {}) {
        std::vector<std::uint8_t> d{0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, addr, a};
        if (b) d.push_back(*b);
        d.push_back(0xF7);
        synth.midi(d);
    }
    double note_rms() {
        render(44100);  // let the effect ramps settle
        note_on(0, 60, 100);
        const auto n = render(22050);
        note_off(0, 60);
        return test::rms_db(n);
    }
};

} // namespace

TEST_CASE("fx_variation_insertion_routing") {
    HVYXG_NEED_TABLES();
    if (!test::have_banks()) return;
    for (const bool syxg50 : {true, false}) {
        CAPTURE(syxg50);
        // Reference: no insertion (the XG default, part off).
        const double dry = SongRig(syxg50).note_rms();
        // Part 1 inserted into "No Effect": the part goes only to the (off) variation.
        SongRig off(syxg50);
        off.xg(0x5B, 0);
        off.xg(0x40, 0x00, 0x00);
        CHECK(off.note_rms() < dry - 60);
        // Thru: volume, expression and pan move behind the effect; the level is unchanged.
        SongRig thru(syxg50);
        thru.xg(0x5B, 0);
        thru.xg(0x40, 0x40, 0x00);
        CHECK(thru.note_rms() == doctest::Approx(dry).epsilon(0.01));
        // System connection with variation send 0: the part plays dry as before.
        SongRig sys(syxg50);
        sys.xg(0x5A, 1);
        CHECK(sys.note_rms() == doctest::Approx(dry).epsilon(0.002));
    }
}

namespace {

/// FxRig with two parts, for the insertion connection.
struct PartRig {
    midi::xg::EffectData ram{test::tables().get()};
    std::array<midi::xg::MultiPart, 2> mp{midi::xg::MultiPart{0}, midi::xg::MultiPart{1}};
    std::array<midi::Part, 2> parts{};
    midi::xg::EffectInsertions ins = test::insertions();
    midi::xg::System sys;
    std::unique_ptr<SystemEffects> fx;
    std::size_t block;
    Buses buses;

    explicit PartRig(bool syxg50) : block(syxg50 ? 128 : 64) {
        ram.reset();
        for (std::size_t i = 0; i < 2; ++i) {
            parts[i].id = i;
            parts[i].ram = &mp[i];
        }
        ModuleContext ctx;
        ctx.tables = test::tables().get();
        ctx.shared.effect1 = &ram;
        ctx.shared.effect_insertion = &ins;
        ctx.shared.system = &sys;
        ctx.shared.parts = parts.data();
        ctx.shared.part_count = 2;
        ctx.sample_rate = 44100.0f;
        fx = syxg50 ? syxg50::make_system_effects(ctx) : syxg2006le::make_system_effects(ctx);
    }
    const fx::SystemFx& sfx() const { return dynamic_cast<const fx::SystemFx&>(*fx); }
    std::vector<float> run(std::size_t frames, const std::function<void(Buses&, std::size_t)>& feed = {}) {
        std::vector<float> left;
        for (std::size_t t = 0; t < frames; t += block) {
            buses.clear();
            if (feed) feed(buses, t);
            fx->process(buses, block);
            left.insert(left.end(), buses.dry_l.begin(), buses.dry_l.begin() + static_cast<long>(block));
        }
        return left;
    }
    static double energy(const std::vector<float>& x, std::size_t from = 0) {
        double e = 0;
        for (std::size_t i = from; i < x.size(); ++i) e += double(x[i]) * x[i];
        return e;
    }
};

} // namespace

TEST_CASE("fx_variation_syxg50_insertion_return_updates") {
    HVYXG_NEED_TABLES();
    // syxg50.dll (register log, renders −108 … −120 dB; before −18 … −31 dB): a volume
    // change of the insertion part moves the return targets at once, with the reverb / chorus
    // sends of its last voice update; the voices' first block moves them before that block's
    // tick, with the current sends.
    PartRig r(true);
    for (const std::uint8_t a : {0x20, 0x21}) r.ram.write(a, 0);  // no chorus
    r.ram.write(0x40, 0x40);  // thru
    r.ram.write(0x41, 0x00);
    r.ram.write(0x5B, 0);
    r.run(44100);
    const float ret0 = r.sfx().variation_return(false);
    const float rev0 = r.sfx().variation_to_reverb(false);
    CHECK(rev0 > 0.0f);  // part reverb send 40
    r.mp[0].reverb_send = 0;  // not applied until the voices sound
    r.mp[0].volume = 127;
    r.run(44100);
    CHECK(r.sfx().variation_return(false) > ret0 * 1.3f);
    CHECK(r.sfx().variation_to_reverb(false) > rev0);
    // The first block with input: the send target drops to 0 and the block's tick slews.
    const float rev1 = r.sfx().variation_to_reverb(false);
    r.run(r.block * 4, [](Buses& b, std::size_t) { b.variation_l[0] = b.variation_r[0] = 0.1f; });
    CHECK(r.sfx().variation_to_reverb(false) < rev1);
}

TEST_CASE("fx_variation_insertion_part_and_connection_writes") {
    HVYXG_NEED_TABLES();
    // syxg50.dll (Process=1 renders, organ chord and Delay L,C,R tail, matched to −90 …
    // −128 dB): an insertion-part write keeps the variation's ring, input gain and control
    // offset, and the return follows the new part at once (a part of 7F keeps the old
    // return); a connection write, even of the same value, re-sends the type: the ring is
    // cleared, the input gain ramps from 0 and the control offset restarts. The same
    // controller code serves the 2006LE's superset variation.
    for (const bool syxg50 : {true, false}) {
        CAPTURE(syxg50);
        PartRig r(syxg50);
        r.mp[1].volume = 50;
        for (const std::uint8_t a : {0x00, 0x01, 0x20, 0x21}) r.ram.write(a, 0);  // no reverb / chorus
        r.ram.write(0x40, 0x05);  // Delay L,C,R
        r.ram.write(0x41, 0x00);
        r.ram.write(0x5A, 0);
        r.ram.write(0x5B, 0);
        r.run(44100);
        CHECK(r.sfx().variation_input_gain() == doctest::Approx(1.0));
        const float ret0 = r.sfx().variation_return(false);
        CHECK(ret0 > 0.0f);
        // An impulse, then a part write before its first echo (C 700 ms).
        const auto feed = [](Buses& b, std::size_t t) { if (t == 0) b.variation_l[0] = b.variation_r[0] = 0.25f; };
        r.run(4096, feed);
        r.ram.write(0x5B, 1);
        auto tail = r.run(44100);
        CHECK(r.sfx().variation_input_gain() == doctest::Approx(1.0));
        CHECK(r.sfx().variation_return(false) < ret0 * 0.5f);  // part 1's volume 50, at once
        CHECK(PartRig::energy(tail) > 1e-5);
        // Part off: the return stays.
        const float ret1 = r.sfx().variation_return(false);
        r.ram.write(0x5B, 0x7F);
        r.run(r.block);
        CHECK(r.sfx().variation_return(false) == ret1);
        // A connection write of the same value clears the ring and restarts the input gain
        // (S-YXG50; the 2006LE, whose variation is silent, takes it as no change).
        r.ram.write(0x5B, 0);
        r.run(44100);
        r.run(4096, feed);
        r.ram.write(0x5A, 0);
        tail = r.run(44100);
        if (syxg50) CHECK(PartRig::energy(tail) == 0.0);
        else CHECK(PartRig::energy(tail) > 1e-5);
        CHECK(r.sfx().variation_input_gain() == doctest::Approx(1.0));
        r.ram.write(0x5A, 0);
        r.run(r.block);
        if (syxg50) CHECK(r.sfx().variation_input_gain() < 0.1f);
        else CHECK(r.sfx().variation_input_gain() == doctest::Approx(1.0));
        // A connection change restarts it on both.
        r.ram.write(0x5A, 1);
        r.run(r.block);
        r.ram.write(0x5A, 0);
        r.run(r.block);
        CHECK(r.sfx().variation_input_gain() < 0.1f);
    }
    // The control offset (syxg50: distortion drive 40 + 63 via MW at depth 127 on both
    // parts) survives a part change and restarts after a connection write.
    PartRig r(true);
    for (auto& p : r.parts) p.controller.modulation = 64;
    r.ram.write(0x40, 0x49);
    r.ram.write(0x41, 0x00);
    r.ram.write(0x5A, 0);
    r.ram.write(0x5B, 0);
    r.ram.write(0x5C, 127);
    r.run(44100 / 2);
    CHECK(r.sfx().control().parameter(Block::Variation, 0) == 103);
    r.ram.write(0x5B, 1);
    r.run(r.block);
    CHECK(r.sfx().control().parameter(Block::Variation, 0) == 103);
    r.ram.write(0x5A, 0);
    r.run(r.block);
    CHECK(r.sfx().control().parameter(Block::Variation, 0) < 50);
    r.run(44100 / 2);
    CHECK(r.sfx().control().parameter(Block::Variation, 0) == 103);
}

TEST_CASE("fx_variation_insertion_part_write_mid_note") {
    HVYXG_NEED_TABLES();
    // syxg50.dll (Process=1, organ chord, reverb and chorus off): an insertion-part write
    // while the part sounds silences it within the block of the write (all sound off), the
    // output is exactly 0 afterwards; a later note sounds again. The 2006LE keeps sounding.
    if (!test::have_banks()) return;
    for (const bool syxg50 : {true, false}) {
        CAPTURE(syxg50);
        SongRig r(syxg50);
        r.xg(0x00, 0x00, 0x00);
        r.xg(0x20, 0x00, 0x00);
        r.xg(0x5B, 0);
        r.xg(0x40, 0x40, 0x00);
        r.program(0, 16);
        r.render(44100);
        for (const std::uint8_t k : {60, 64, 67}) r.note_on(0, k, 100);
        const double before = test::rms_db(r.render(22050));
        r.xg(0x5B, 0);
        r.render(256);
        const auto after = r.render(22050);
        if (syxg50) {
            CHECK(std::all_of(after.begin(), after.end(), [](float x) { return x == 0.0f; }));
            r.note_on(0, 72, 100);
            CHECK(test::rms_db(r.render(22050)) > before - 12);
        } else {
            CHECK(test::rms_db(after) == doctest::Approx(before).epsilon(0.02));
        }
    }
}
