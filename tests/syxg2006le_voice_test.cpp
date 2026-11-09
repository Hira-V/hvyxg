// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// S-YXG2006LE tone module (engine/syxg2006le). Table and law checks run always; the real-bank
// cases read HVYXG_2006LE_BIN / HVYXG_2006LE_DATA and are skipped when they are unset. The
// expected values were measured from renders of the real S-YXG2006LE.DLL.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <vector>

#include "hvyxg/bank.hpp"
#include "midi/part.hpp"
#include "syxg2006le/bank_data.hpp"
#include "syxg2006le/controllers.hpp"
#include "syxg2006le/dsp_voice.hpp"
#include "syxg2006le/tbl/dll_tables.hpp"
#include "test_support.hpp"

using namespace hvyxg;
namespace dll = hvyxg::syxg2006le::tbl::dll;

namespace {

std::shared_ptr<const Bank> real_bank() { return test::bank(false); }

struct Rig : test::Rig {
    explicit Rig(std::shared_ptr<const Bank> bank, std::uint16_t polyphony = 32) : test::Rig(std::move(bank), polyphony) {}
    void note(std::uint8_t ch, std::uint8_t key, std::uint8_t vel) { note_on(ch, key, vel); }
    std::vector<float> render(double seconds) { return render_left(seconds); }
};

double rms(const std::vector<float>& x, std::size_t a, std::size_t b) {
    double s = 0;
    for (std::size_t i = a; i < b; ++i) s += static_cast<double>(x[i]) * x[i];
    return std::sqrt(s / static_cast<double>(b - a));
}

/// Frequency from the autocorrelation peak of x[a, a + n) between periods lo..hi (samples),
/// refined by parabolic interpolation.
double frequency(const std::vector<float>& x, std::size_t a, std::size_t n, int lo, int hi) {
    const auto ac = [&](int lag) {
        double s = 0;
        for (std::size_t i = 0; i < n; ++i) s += static_cast<double>(x[a + i]) * x[a + i + static_cast<std::size_t>(lag)];
        return s;
    };
    int best = lo;
    double bv = -1e300;
    for (int l = lo; l <= hi; ++l) {
        const double v = ac(l);
        if (v > bv) {
            bv = v;
            best = l;
        }
    }
    const double y0 = ac(best - 1), y1 = bv, y2 = ac(best + 1);
    const double d = 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2);
    return 44100.0 / (best + d);
}

} // namespace

TEST_CASE("syxg2006le_dll_tables") {
    HVYXG_NEED_TABLES();
    // Send / dry levels: T[127] = 0, T[64] = 16 units, T[0] = 255 (off).
    CHECK(test::tables()->le.kSendAtt[254] == 0);
    CHECK(test::tables()->le.kSendAtt[128] == 16);
    CHECK(test::tables()->le.kSendAtt[0] == 255);
    // Volume / drum level: ≈ −32·log2(i/256).
    CHECK(test::tables()->le.kVolumeAtt[255] == 0);
    CHECK(test::tables()->le.kVolumeAtt[128] == 32);
    CHECK(test::tables()->le.kVolumeAtt[0] == 257);
    // Detune: centre 0x80, −50 .. +49 cents.
    CHECK(test::tables()->le.kDetune[0x80] == 0);
    CHECK(test::tables()->le.kDetune[0] == -50);
    // Controller amplitude: depth 64 is neutral, depth 0 attenuates fully at value 127.
    CHECK(test::tables()->le.kAmpControl[64][127] == 0);
    CHECK(test::tables()->le.kAmpControl[0][127] == 257);
    CHECK(test::tables()->le.kAmpControl[127][127] == -test::tables()->le.kAmpControl[0][127]);
    // Filter ceiling: resonance 16 caps LPF at 0xF6000 (f = 0.6875), ≥ 32 at 0xFFF00.
    CHECK(test::tables()->le.kFilterCeiling[0][16] == 0xF60);
    CHECK(test::tables()->le.kFilterCeiling[0][32] == 0xFFF);
}

TEST_CASE("syxg2006le_engine_tables") {
    HVYXG_NEED_TABLES();
    const syxg2006le::RateTables t44(test::tables()->le, 44100.0f);
    // Decay rate 36: 2^26 / 349504 = 192 per tick (12.15 dB/s, measured 12.08 – 12.26).
    CHECK(t44.inc_decay[36] == 192);
    // Release table = decay table shifted by one for rates 1..95.
    CHECK(t44.inc_release[37] == t44.inc_decay[37]);
    CHECK(t44.inc_release[0] == 0);
    // Pitch mantissas: 44.1 kHz → 1.0 at index 0; 48 kHz scales by 44100/48000.
    CHECK(t44.pitch[0] == doctest::Approx(1.0));
    const syxg2006le::RateTables t48(test::tables()->le, 48000.0f);
    CHECK(t48.pitch[0] == doctest::Approx(44100.0 / 48000.0));
    CHECK(t48.cutoff_base == -8012);
    // Output-stage glides: pan 2 units, sends 8 units per tick at 44.1 kHz.
    CHECK(t44.inc_pan == 8192);
    CHECK(t44.inc_send == 32768);
    CHECK(t44.gate_samples == 1323);
}

TEST_CASE("syxg2006le_controller_sums_defaults") {
    midi::xg::MultiPart ram(0);
    midi::Part part;
    part.ram = &ram;
    midi::xg::System sys{};
    const auto src = syxg2006le::controller_sources(part);
    const auto s = syxg2006le::controller_sums(test::tables()->le, part, src, sys, 60, false);
    // Volume 100, expression 127, master volume 127: index 200 → 11 units.
    CHECK(s.amp == test::tables()->le.kVolumeAtt[200]);
    CHECK(s.pitch == 0);
    CHECK(s.filter == 0);
    CHECK(s.pmod == 0);
    // Mod wheel 127 with the default MW LFO PMod depth 10: 128·10·10 >> 7 = 100 cents p-p.
    part.controller.modulation = 127;
    const auto mw = syxg2006le::controller_sums(test::tables()->le, 
        part, syxg2006le::controller_sources(part), sys, 60, false);
    CHECK(mw.pmod == 100);
    // Pitch bend 0x3FFF (centred +0x1FFF) counts as +0x2000: +200 cents at
    // range 2; one step below the centre (0x1FFF) stays −1 (−1 cent: floor of −50/2048).
    part.pitchbend = 0x3FFF;
    const auto pb = syxg2006le::controller_sums(test::tables()->le, 
        part, syxg2006le::controller_sources(part), sys, 60, false);
    CHECK(pb.pitch == 200);
    part.pitchbend = 0x1FFF;
    const auto pbm = syxg2006le::controller_sums(test::tables()->le, 
        part, syxg2006le::controller_sources(part), sys, 60, false);
    CHECK(pbm.pitch == -1);
}

TEST_CASE("syxg2006le_program_fallbacks") {
    const auto bank = real_bank();
    if (!bank) {
        MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    // Drum kits (seg8): MSB 127 → kits 1 (Standard), 2 (Room, P8); MSB 126 → kit 0.
    const auto std_kit = impl(*bank).drum_setup(127, 0);
    REQUIRE(std_kit);
    CHECK((*std_kit)[38 - 13].pitch_coarse != 0); // snare present
    CHECK(impl(*bank).drum_setup(127, 8));
    CHECK(impl(*bank).drum_setup(126, 0));
    CHECK(!impl(*bank).drum_setup(0, 0)); // a melodic program is no kit
    // A missing kit plays the Standard Kit (measured: MSB 127 P5 = P0).
    CHECK(impl(*bank).drum_setup(127, 5) == std_kit);
    // The GS drum MSB plays the XG kits.
    CHECK(impl(*bank).drum_setup(kDrumBankMsbGs, 0) == std_kit);
}

TEST_CASE("syxg2006le_piano_level_and_pitch") {
    HVYXG_NEED_TABLES();
    const auto bank = real_bank();
    if (!bank) {
        MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    Rig rig(bank);
    // The reference is a dry render: no reverb send.
    rig.cc(0, 91, 0);
    rig.render(0.1);
    rig.note(0, 60, 127);
    const auto loud = rig.render(1.0);
    rig.note_off(0, 60);
    rig.render(1.0);
    rig.note(0, 60, 64);
    const auto soft = rig.render(1.0);
    // DLL (dry render, 0.2 – 1.0 s after the note-on): RMS 0.02940 / 0.00742 (11.96 dB).
    const double a = rms(loud, 8820, 44100), b = rms(soft, 8820, 44100);
    CHECK(20 * std::log10(a / 0.02940) == doctest::Approx(0.0).epsilon(0.05));
    CHECK(20 * std::log10(a / b) == doctest::Approx(11.96).epsilon(0.005));
}

TEST_CASE("syxg2006le_organ_pitch_bend") {
    HVYXG_NEED_TABLES();
    const auto bank = real_bank();
    if (!bank) {
        MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    Rig rig(bank);
    rig.program(0, 16);
    rig.render(0.1);
    rig.note(0, 69, 100);
    const auto flat = rig.render(0.6);
    rig.bend(0, 0x3FFF);
    const auto bent = rig.render(0.6);
    // Organ note 69 (220 Hz fundamental in this voice): bend +8191 at range 2 = +199.1 cents
    // measured from the DLL.
    const double f0 = frequency(flat, 4410, 16384, 150, 250);
    const double f1 = frequency(bent, 4410, 16384, 120, 220);
    CHECK(f0 == doctest::Approx(220.0).epsilon(0.002));
    CHECK(1200.0 * std::log2(f1 / f0) == doctest::Approx(199.0).epsilon(0.01));
}

TEST_CASE("syxg2006le_polyphony_limit") {
    HVYXG_NEED_TABLES();
    const auto bank = real_bank();
    if (!bank) {
        MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    Rig rig(bank, 32);
    for (std::uint8_t k = 30; k < 90; ++k) rig.note(0, k, 100);
    const auto x = rig.render(0.2);
    CHECK(rms(x, 0, x.size()) > 0.0);
    CHECK(rig.synth.active());
}

namespace {

double correlation(const std::vector<float>& a, const std::vector<float>& b) {
    double ab = 0, aa = 0, bb = 0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        ab += double(a[i]) * b[i];
        aa += double(a[i]) * a[i];
        bb += double(b[i]) * b[i];
    }
    return ab / std::sqrt(aa * bb + 1e-30);
}

constexpr std::initializer_list<std::uint8_t> kGsReset = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7};

} // namespace

TEST_CASE("syxg2006le_gs_mode_plays_the_gs_banks") {
    HVYXG_NEED_TABLES();
    const auto bank = real_bank();
    if (!bank) {
        MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    // The DLL's GS layer plays the rhythm part from MSB 118 (GS kits) and melodic parts from MSB
    // 119 with CC#0 as LSB: a GS render equals the XG render of those banks (measured
    // bit-identical on the DLL for channel 10, CC#0 0 + program 16 after a GS reset).
    const auto play = [&](bool gs, std::uint8_t ch, std::uint8_t msb, std::uint8_t lsb,
                          std::uint8_t prog, std::uint8_t key) {
        Rig rig(bank);
        if (gs) rig.sysex(kGsReset);
        rig.cc(ch, 91, 0);
        rig.cc(ch, 93, 0);
        rig.cc(ch, 0, msb);
        rig.cc(ch, 32, lsb);
        rig.program(ch, prog);
        rig.render(0.1);
        rig.note(ch, key, 100);
        return rig.render(0.5);
    };
    const auto gs_kit = play(true, 9, 0, 0, 16, 38);
    const auto xg_kit = play(false, 9, 118, 0, 16, 38);
    REQUIRE(rms(xg_kit, 0, xg_kit.size()) > 1e-3);
    CHECK(correlation(gs_kit, xg_kit) > 0.9999);
    CHECK(rms(gs_kit, 0, gs_kit.size()) == doctest::Approx(rms(xg_kit, 0, xg_kit.size())).epsilon(1e-3));
    const auto gs_tone = play(true, 0, 8, 3, 4, 60);  // CC#32 is ignored in GS mode
    const auto xg_tone = play(false, 0, 119, 8, 4, 60);
    REQUIRE(rms(xg_tone, 0, xg_tone.size()) > 1e-3);
    CHECK(correlation(gs_tone, xg_tone) > 0.9999);
}

TEST_CASE("syxg2006le_gs_kit_pitch") {
    HVYXG_NEED_TABLES();
    const auto bank = real_bank();
    if (!bank) {
        MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    // The GS kits (voice type 6) store drum pitch coarse 60 and count it from the pitch key 60;
    // they share the XG Standard Kit's samples, so their notes have its pitch (measured on the
    // DLL: same spectral peaks, only the level differs).
    const auto hit = [&](std::uint8_t msb, std::uint8_t key) {
        Rig rig(bank);
        rig.cc(9, 91, 0);
        rig.cc(9, 0, msb);
        rig.program(9, 0);
        rig.render(0.1);
        rig.note(9, key, 100);
        return rig.render(0.3);
    };
    for (const std::uint8_t key : {36, 70}) {
        const auto gs = hit(118, key);
        const auto xg = hit(127, key);
        REQUIRE(rms(xg, 0, xg.size()) > 1e-4);
        CHECK(correlation(gs, xg) > 0.99);
    }
}

// ---- Note handler: mono, portamento, CC84, soft pedal. Expected values: the same sequences
// rendered by the real DLL (dry, sends 0).

namespace {

struct NoteRig : Rig {
    explicit NoteRig(std::shared_ptr<const Bank> bank, std::uint8_t program) : Rig(std::move(bank)) {
        for (std::uint8_t ch = 0; ch < 16; ++ch)
            for (const std::uint8_t c : {91, 93, 94}) cc(ch, c, 0);
        this->program(0, program);
    }
    /// Render `blocks` 64-sample blocks and append the left channel to `out` (events stay on
    /// block starts, as in the reference renders).
    void run(std::size_t blocks) {
        const auto st = test::Rig::render(blocks * 64);
        for (std::size_t i = 0; i < st.size(); i += 2) out.push_back(st[i]);
    }
    /// The set-up time before the first note (7 blocks).
    void settle() { test::Rig::render(7 * 64); }
    std::vector<float> out;
};

/// Durations in blocks: 0.1 s ≈ 69, 0.5 s ≈ 344, 1.0 s ≈ 689.
constexpr std::size_t k01 = 69, k05 = 344, k10 = 689;

std::size_t at(double t) { return static_cast<std::size_t>(t * 44100.0); }

} // namespace

TEST_CASE("syxg2006le_mono_portamento_legato_and_retrigger") {
    HVYXG_NEED_TABLES();
    const auto bank = real_bank();
    if (!bank) {
        MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    // Organ, mono, portamento time 40: 69, then 81 legato (glide from 69), then 81 released:
    // 69 re-triggers and glides back down from 81.
    NoteRig rig(bank, 16);
    rig.cc(0, 126, 1);
    rig.cc(0, 65, 127);
    rig.cc(0, 5, 40);
    rig.settle();
    rig.note(0, 69, 100);
    rig.run(k05);
    rig.note(0, 81, 100);
    rig.run(k10);
    rig.note_off(0, 81);
    rig.run(k10);
    const auto& x = rig.out;
    // DLL: f0 over 2048-sample windows.
    const std::pair<double, double> expect[] = {{0.20, 220.04}, {0.55, 270.36}, {0.60, 309.77}, {1.20, 440.00},
                                                {1.55, 358.75}, {1.60, 312.95}, {2.20, 220.02}};
    for (const auto& [t, f] : expect) {
        INFO("t = " << t);
        CHECK(frequency(x, at(t), 2048, 40, 250) == doctest::Approx(f).epsilon(0.001));
    }
}

TEST_CASE("syxg2006le_mono_high_note_priority") {
    HVYXG_NEED_TABLES();
    const auto bank = real_bank();
    if (!bank) {
        MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    // XG 0A 00 01 = 1: a lower key under the held 69 only joins the held keys; releasing 69
    // re-triggers it.
    NoteRig rig(bank, 16);
    rig.cc(0, 126, 1);
    rig.xg({0x0A, 0x00, 0x01, 0x01});
    rig.settle();
    rig.note(0, 69, 100);
    rig.run(k05);
    rig.note(0, 64, 100);
    rig.run(k05);
    rig.note_off(0, 69);
    rig.run(k05);
    const auto& x = rig.out;
    CHECK(frequency(x, at(0.2), 8192, 100, 300) == doctest::Approx(220.01).epsilon(0.001));
    CHECK(frequency(x, at(0.7), 8192, 100, 300) == doctest::Approx(220.01).epsilon(0.001));
    CHECK(frequency(x, at(1.2), 8192, 100, 300) == doctest::Approx(164.78).epsilon(0.001));
}

TEST_CASE("syxg2006le_cc84_voice_takeover") {
    HVYXG_NEED_TABLES();
    const auto bank = real_bank();
    if (!bank) {
        MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    // Poly organ: CC84 69, then 71 within an octave takes the sounding 69's voices over: the
    // pitch jumps, no second note sounds.
    NoteRig rig(bank, 16);
    rig.settle();
    rig.note(0, 69, 100);
    rig.run(k05);
    rig.cc(0, 84, 69);
    rig.note(0, 71, 100);
    rig.run(k05);
    const auto& x = rig.out;
    CHECK(frequency(x, at(0.7), 8192, 100, 300) == doctest::Approx(246.83).epsilon(0.001));
    // One voice continues at the level of the first note (DLL RMS 0.02148 before and after).
    CHECK(rms(x, at(0.55), at(0.95)) == doctest::Approx(0.02148).epsilon(0.005));
}

TEST_CASE("syxg2006le_poly_portamento_from_a_gliding_note") {
    HVYXG_NEED_TABLES();
    const auto bank = real_bank();
    if (!bank) {
        MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    // Poly, portamento time 70: 57 held, 69 glides up from 57, 81 starts 0.1 s later from
    // where 69 is (its pitch key + its current glide offset).
    NoteRig rig(bank, 16);
    rig.cc(0, 65, 127);
    rig.cc(0, 5, 70);
    rig.settle();
    rig.note(0, 57, 100);
    rig.run(k05);
    rig.note(0, 69, 100);
    rig.run(k01);
    rig.note(0, 81, 100);
    rig.run(k05);
    const auto& x = rig.out;
    const std::pair<double, double> expect[] = {{0.55, 227.79}, {0.62, 248.52}, {0.65, 256.32}, {0.70, 270.72}};
    for (const auto& [t, f] : expect) {
        INFO("t = " << t);
        CHECK(frequency(x, at(t), 2048, 40, 250) == doctest::Approx(f).epsilon(0.001));
    }
}

TEST_CASE("syxg2006le_soft_pedal") {
    HVYXG_NEED_TABLES();
    const auto bank = real_bank();
    if (!bank) {
        MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    // GM piano note 72 at velocity 40, then again with CC67 127 at its note-on: the DLL's soft
    // note is −1.127 dB in RMS and −1.641 dB after a first difference (the cutoff curve) over
    // 50–250 ms after each note-on.
    NoteRig rig(bank, 0);
    rig.settle();
    rig.note(0, 72, 40);
    rig.run(k05);
    rig.note_off(0, 72);
    rig.run(k01);
    rig.cc(0, 67, 127);
    rig.run(k01);
    rig.note(0, 72, 40);
    rig.run(k05);
    const auto& x = rig.out;
    std::vector<float> d(x.size() - 1);
    for (std::size_t i = 0; i + 1 < x.size(); ++i) d[i] = x[i + 1] - x[i];
    const std::size_t o = (k05 + 2 * k01) * 64;
    const double a = rms(x, at(0.05), at(0.25)), b = rms(x, o + at(0.05), o + at(0.25));
    const double ha = rms(d, at(0.05), at(0.25)), hb = rms(d, o + at(0.05), o + at(0.25));
    CHECK(20 * std::log10(b / a) == doctest::Approx(-1.127).epsilon(0.03));
    CHECK(20 * std::log10(hb / ha) == doctest::Approx(-1.641).epsilon(0.03));
}

namespace {

/// A controller step on a held organ note (program 16, note 69, pan 64, sends 0), rendered with
/// and without the step. Setup at frame 4410, note-on at 13230, the step at `step_frame`; the
/// frames match the DLL probes (1 tick = 1 frame).
struct StepProbe {
    std::vector<float> with, without;
    std::size_t block = 0;

    StepProbe(const std::shared_ptr<const Bank>& bank, std::uint8_t depth_addr, std::uint8_t depth,
              std::size_t step_frame, const std::vector<std::uint8_t>& step) {
        block = step_frame / 64;
        for (int pass = 0; pass < 2; ++pass) {
            using Msg = std::vector<std::uint8_t>;
            const auto xg = [](std::uint8_t addr, std::uint8_t v) {
                return Msg{0xF0, 0x43, 0x10, 0x4C, 0x08, 0x00, addr, v, 0xF7};
            };
            std::vector<std::pair<std::size_t, Msg>> events = {
                {4410, {0xC0, 16}},   {4420, {0xB0, 10, 64}}, {4430, {0xB0, 91, 0}},
                {4440, {0xB0, 93, 0}}, {4450, {0xB0, 94, 0}},
                {4460, xg(0x20, 0)},   // MW LFO PMod depth 0
                {4470, xg(0x23, 0x40)}, // PB pitch 0
                {4480, xg(depth_addr, depth)},
                {13230, {0x90, 69, 127}}};
            if (pass == 0) events.emplace_back(step_frame, step);
            // As render_yxg: each event at its frame offset within the next render call.
            Rig rig(bank);
            std::vector<float> out;
            std::size_t next = 0;
            const std::size_t end = step_frame + 160 * 64;
            while (out.size() < end) {
                const std::size_t n = std::min<std::size_t>(end - out.size(), 4096);
                for (; next < events.size() && events[next].first < out.size() + n; ++next)
                    rig.synth.midi(events[next].second, static_cast<std::uint32_t>(events[next].first - out.size()));
                const auto st = rig.test::Rig::render(n);
                for (std::size_t i = 0; i < st.size(); i += 2) out.push_back(st[i]);
            }
            (pass == 0 ? with : without) = std::move(out);
        }
    }
    /// First block (relative to the step's block) where the two renders differ.
    int first_change() const {
        for (std::size_t i = (block - 2) * 64; i < with.size(); ++i)
            if (with[i] != without[i]) return static_cast<int>(i / 64) - static_cast<int>(block);
        return 1000;
    }
    /// RMS of the difference over blocks [block + a, block + b).
    double diff(std::size_t a, std::size_t b) const {
        double s = 0;
        for (std::size_t i = (block + a) * 64; i < (block + b) * 64; ++i) {
            const double d = static_cast<double>(with[i]) - without[i];
            s += d * d;
        }
        return std::sqrt(s / static_cast<double>((b - a) * 64));
    }
};

/// Checks a probe against the DLL: the first changed block and the RMS of the difference over
/// blocks 0–8, 8–32 and 32–128 after the step's block (left channel).
void check_step(const StepProbe& p, int first, double r0, double r1, double r2) {
    CHECK(p.first_change() == first);
    CHECK(p.diff(0, 8) == doctest::Approx(r0).epsilon(1e-4));
    CHECK(p.diff(8, 32) == doctest::Approx(r1).epsilon(1e-4));
    CHECK(p.diff(32, 128) == doctest::Approx(r2).epsilon(1e-4));
}

} // namespace

// Controller transitions (the controller timer): the 12 ms flush runs after
// the block's events and before its voices, so a step acts in the first block whose end
// crosses the next 12 ms boundary; the DLL's values (DLL renders minus no-step renders) below.
// Frame 35297 (block 551): the flush acts three blocks later; frame 35509 (block 554): in the
// event's own block.
TEST_CASE("syxg2006le_controller_step_mw_lfo_pitch_depth") {
    HVYXG_NEED_TABLES();
    const auto bank = real_bank();
    if (!bank) {
        MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    // MW LFO PMod depth 127 (PMod 1270 → the 400-cent limit), CC1 0 → 127.
    check_step(StepProbe(bank, 0x20, 127, 35297, {0xB0, 1, 127}), 3, 0.022212, 0.0542878, 0.043503);
    check_step(StepProbe(bank, 0x20, 127, 35509, {0xB0, 1, 127}), 0, 0.0327718, 0.0558655, 0.0434606);
}

TEST_CASE("syxg2006le_controller_step_ac1_filter") {
    HVYXG_NEED_TABLES();
    const auto bank = real_bank();
    if (!bank) {
        MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    // AC1 (CC16) filter control 0x60 (+2400 cents), 0 → 127: the cutoff glides 3072 / block;
    // the SVF coefficient ramps from the previous block's value at the block's first sample.
    check_step(StepProbe(bank, 0x5B, 0x60, 35297, {0xB0, 16, 127}), 3, 0.00131605, 0.00716419, 0.0102615);
    check_step(StepProbe(bank, 0x5B, 0x60, 35509, {0xB0, 16, 127}), 0, 0.00251885, 0.00787506, 0.0102712);
}

TEST_CASE("syxg2006le_controller_step_cat_amp") {
    HVYXG_NEED_TABLES();
    const auto bank = real_bank();
    if (!bank) {
        MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    // Channel aftertouch amplitude control 0x20, 0 → 127: the level glides 3 units / block.
    check_step(StepProbe(bank, 0x4F, 0x20, 35297, {0xD0, 127}), 3, 0.00918298, 0.0302437, 0.0356276);
    check_step(StepProbe(bank, 0x4F, 0x20, 35509, {0xD0, 127}), 0, 0.0160795, 0.0317473, 0.0356718);
}

TEST_CASE("syxg2006le_controller_step_bend") {
    HVYXG_NEED_TABLES();
    const auto bank = real_bank();
    if (!bank) {
        MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    // Bend 0x2000 → 0x3FFF at range 2 (+0x1FFF counts as +0x2000: +200 cents).
    check_step(StepProbe(bank, 0x23, 0x42, 35297, {0xE0, 0x7F, 0x7F}), 3, 0.0331418, 0.0502876, 0.0516091);
    check_step(StepProbe(bank, 0x23, 0x42, 35509, {0xE0, 0x7F, 0x7F}), 0, 0.0475294, 0.0499164, 0.0511753);
}

namespace {

/// The voice-manager probe measured against the DLL: claves (a short one-shot, ended after
/// ≈ 26 ms but still holding its entry), 30 organ notes panned left and one panned right fill
/// the 32 entries; a 33rd note at `t_new` steals the right one (all scores 1, ties to the
/// highest entry) unless the score refresh at 120 ms (the 10th controller flush, in the block
/// [5248, 5312)) has freed the claves' entry first. Returns the right channel's RMS over
/// 0.2 … 0.4 s.
double steal_probe(const std::shared_ptr<const Bank>& bank, double t_new) {
    test::Rig rig(bank, 32);
    std::vector<float> out;
    std::size_t done = 0;
    const auto until = [&](double t) {
        // Events act from the 64-frame block that holds them, as in the DLL.
        const auto frames = static_cast<std::size_t>(t * 44100.0) / 64 * 64;
        if (frames > done) {
            const auto x = rig.render(frames - done);
            out.insert(out.end(), x.begin(), x.end());
            done = frames;
        }
    };
    until(0.01);
    for (std::uint8_t ch = 0; ch < 16; ++ch) {
        rig.cc(ch, 91, 0);
        rig.cc(ch, 93, 0);
    }
    rig.program(0, 16);
    rig.program(2, 16);
    rig.cc(0, 10, 1);
    rig.cc(2, 10, 127);
    until(0.03);
    rig.note_on(9, 75, 100);
    until(0.032);
    for (std::uint8_t k = 40; k < 70; ++k) rig.note_on(0, k, 100);
    until(0.034);
    rig.note_on(2, 72, 100);
    until(t_new);
    rig.note_on(0, 80, 100);
    until(0.45);
    double s = 0;
    const std::size_t a = 8820, b = 17640;
    for (std::size_t i = a; i < b; ++i) s += double(out[2 * i + 1]) * out[2 * i + 1];
    return std::sqrt(s / double(b - a));
}

} // namespace

TEST_CASE("syxg2006le_ended_voice_holds_its_entry_until_the_score_refresh") {
    HVYXG_NEED_TABLES();
    const auto bank = real_bank();
    if (!bank) {
        MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    // DLL (64-frame host blocks): stolen at 100 / 118 ms, kept at 121 / 140 ms (-31.4 dB).
    CHECK(steal_probe(bank, 0.100) == 0.0);
    CHECK(steal_probe(bank, 0.118) == 0.0);
    CHECK(20.0 * std::log10(steal_probe(bank, 0.121)) == doctest::Approx(-31.4).epsilon(0.01));
    CHECK(20.0 * std::log10(steal_probe(bank, 0.140)) == doctest::Approx(-31.4).epsilon(0.01));
}

TEST_CASE("syxg2006le_pool_object_lfo_offsets") {
    // index·65·2^20 as a 32-bit signed product divided by 360 (measured in the
    // DLL's pool: object 31 → 626232, object 32 → 419431, object 63 → 1045664).
    hvyxg::syxg2006le::DspVoice v;
    v.init_object(31);
    CHECK(v.lfo_offset() == 626232u);
    v.init_object(32);
    CHECK(v.lfo_offset() == 419431u);
    v.init_object(63);
    CHECK(v.lfo_offset() == 1045664u);
}

TEST_CASE("syxg2006le_mono_legato_inside_a_slow_attack") {
    HVYXG_NEED_TABLES();
    // Legato (mono + portamento) 50 ms into a strings attack lengthened by CC73 = 100: the new
    // voice's attack rate is re-derived from the old voice's level, which the DLL reads from
    // the gain at the start of its last tick. DLL: the
    // left channel at 0.4 … 0.6 s is −40.096 dB (with the current gain: 0.7 dB louder).
    const auto bank = real_bank();
    if (!bank) {
        MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    test::Rig rig(bank, 32);
    std::vector<float> out;
    std::size_t done = 0;
    const auto until = [&](double t) {
        // Events act from the 64-frame block that holds them, as in the DLL.
        const auto frames = static_cast<std::size_t>(t * 44100.0) / 64 * 64;
        if (frames > done) {
            const auto st = rig.render(frames - done);
            for (std::size_t i = 0; i < st.size(); i += 2) out.push_back(st[i]);
            done = frames;
        }
    };
    until(0.01);
    rig.program(0, 48);
    rig.cc(0, 91, 0);
    rig.cc(0, 93, 0);
    rig.cc(0, 10, 64);
    until(0.02);
    rig.cc(0, 126, 1);
    rig.cc(0, 73, 100);
    rig.cc(0, 65, 127);
    rig.cc(0, 5, 20);
    until(0.1);
    rig.note_on(0, 60, 100);
    until(0.15);
    rig.note_on(0, 64, 100);
    until(0.17);
    rig.note_off(0, 60);
    until(0.7);
    CHECK(20.0 * std::log10(rms(out, 17640, 26460)) == doctest::Approx(-40.096).epsilon(0.0005));
}

TEST_CASE("syxg2006le_lfo_fade_in_runs_against_the_delay_timer") {
    HVYXG_NEED_TABLES();
    // the fade-in factor is counter / fade, but the stage ends when the counter
    // reaches the LFO *delay* length. Soprano sax (fade 2 s) with XG vibrato delay 0 and depth
    // 127: no delay, so the vibrato is at full depth (±200 cents) once the 30 ms gate opens.
    // DLL: −170 … +179 cents over 0.2 … 0.6 s (a 2 s fade would give about ±40).
    const auto bank = real_bank();
    if (!bank) {
        MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    Rig rig(bank, 32);
    rig.program(0, 64);
    rig.cc(0, 91, 0);
    rig.cc(0, 93, 0);
    for (const auto& [lsb, v] : {std::pair<std::uint8_t, std::uint8_t>{9, 127}, {10, 0}}) {
        rig.cc(0, 99, 1);
        rig.cc(0, 98, lsb);
        rig.cc(0, 6, v);
    }
    rig.note(0, 60, 100);
    const auto x = rig.render(0.7);
    double lo = 1e9, hi = -1e9;
    for (double t = 0.1; t < 0.5; t += 0.01) {
        const double f = frequency(x, static_cast<std::size_t>(t * 44100.0), 1024, 100, 250);
        const double c = 1200.0 * std::log2(f / 261.63);
        lo = std::min(lo, c);
        hi = std::max(hi, c);
    }
    CHECK(lo < -150.0);
    CHECK(hi > 150.0);
}

TEST_CASE("syxg2006le_filter_ramp_starts_at_the_note_on_cutoff") {
    HVYXG_NEED_TABLES();
    // the filter is set up at note-on from the cutoff with the filter EG at its
    // start level, and the first block's coefficient ramps from there to the value after the
    // first EG tick. Synth brass (program 62, a fast filter EG), note-on block, left channel:
    // DLL −45.9559 dB (starting the ramp at the first tick's value: −46.010 dB).
    const auto bank = real_bank();
    if (!bank) {
        MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    Rig rig(bank, 32);
    rig.program(0, 62);
    rig.cc(0, 91, 0);
    rig.cc(0, 93, 0);
    rig.cc(0, 10, 64);
    rig.note(0, 60, 100);
    const auto x = rig.render(0.01);
    CHECK(20.0 * std::log10(rms(x, 0, 64)) == doctest::Approx(-45.9559).epsilon(0.00005));
}

TEST_CASE("syxg2006le_xg_attack_change_reaches_sounding_voices") {
    HVYXG_NEED_TABLES();
    // CC73 re-derives the attack of the part's sounding voices. Strings with
    // CC73 = 100 (slow attack), note at 0.2 s, CC73 = 40 at 0.25 s: DLL −30.514 dB at
    // 0.3 … 0.5 s (with the attack left slow: −53.9 dB).
    const auto bank = real_bank();
    if (!bank) {
        MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    test::Rig rig(bank, 32);
    std::vector<float> out;
    const auto until = [&](double t) {
        // Events act from the 64-frame block that holds them, as in the DLL.
        const auto frames = static_cast<std::size_t>(t * 44100.0) / 64 * 64;
        if (frames > out.size()) {
            const auto st = rig.render(frames - out.size());
            for (std::size_t i = 0; i < st.size(); i += 2) out.push_back(st[i]);
        }
    };
    until(0.01);
    rig.program(0, 48);
    rig.cc(0, 91, 0);
    rig.cc(0, 93, 0);
    rig.cc(0, 73, 100);
    until(0.2);
    rig.note_on(0, 60, 100);
    until(0.25);
    rig.cc(0, 73, 40);
    until(0.55);
    CHECK(20.0 * std::log10(rms(out, 13230, 22050)) == doctest::Approx(-30.514).epsilon(0.0005));
}

namespace {

/// Render the left channel, sending each event at the start of the 64-frame block that holds
/// its time (as the DLL applies it).
struct Timeline {
    test::Rig& rig;
    std::vector<float> out;
    void until(double t) {
        const auto frames = static_cast<std::size_t>(t * 44100.0) / 64 * 64;
        if (frames <= out.size()) return;
        const auto st = rig.render(frames - out.size());
        for (std::size_t i = 0; i < st.size(); i += 2) out.push_back(st[i]);
    }
};

} // namespace

TEST_CASE("syxg2006le_sustain_pedal_reaches_released_voices") {
    HVYXG_NEED_TABLES();
    // a pedal change re-derives the release rate of released voices (damper
    // mode 3: the damper release rate while the pedal is down). Piano note-off at 0.3 s,
    // pedal down at 0.4 s: DLL −49.469 dB at 0.6 … 0.8 s (pedal-up release: −79.2 dB).
    const auto bank = real_bank();
    if (!bank) {
        MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    test::Rig rig(bank, 32);
    Timeline tl{rig, {}};
    rig.cc(0, 91, 0);
    rig.cc(0, 93, 0);
    rig.note_on(0, 60, 100);
    tl.until(0.3);
    rig.note_off(0, 60);
    tl.until(0.4);
    rig.cc(0, 64, 127);
    tl.until(0.85);
    CHECK(20.0 * std::log10(rms(tl.out, 26460, 35280)) == doctest::Approx(-49.469).epsilon(0.0005));
}

TEST_CASE("syxg2006le_type6_detune_scales_by_pitch_key") {
    HVYXG_NEED_TABLES();
    // voice type 6 (MSB 118 / 119) takes the part detune as
    // (index − 0x80)·T[ev[1]] / 4096 cents: detune 0xFF on MSB 119 strings at key 48 is
    // +162 cents (the type-0 table: +49) [meas: −125 … −136 dB at five keys].
    const auto bank = real_bank();
    if (!bank) {
        MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    const auto pitch = [&](std::uint8_t msb, std::uint8_t lsb) {
        Rig rig(bank, 32);
        rig.cc(0, 91, 0);
        rig.cc(0, 93, 0);
        rig.cc(0, 0, 119);
        rig.program(0, 48);
        rig.xg({0x08, 0x00, 0x09, msb});
        rig.xg({0x08, 0x00, 0x0A, lsb});
        rig.note(0, 48, 100);
        const auto x = rig.render(0.6);
        return frequency(x, 13230, 8192, 250, 400);
    };
    const double cents = 1200.0 * std::log2(pitch(0x0F, 0x0F) / pitch(0x08, 0x00));
    CHECK(cents == doctest::Approx(162.0).epsilon(0.02));
}
