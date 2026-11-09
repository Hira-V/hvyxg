// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// SXG-Create-NF MU banks (`enable_mu`, docs/ENGINE.md "MU banks") and DLL-exact mode
// (`dll_exact`). The MU bank cases need HVYXG_MU_BIN / HVYXG_MU_DATA (a big-layout table such
// as SXGMU1KX.TBL / .UPCM made by SXG-Create-NF's main.py) and skip without them.
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <vector>

#include "hvyxg/bank.hpp"
#include "fx/fx_mu.hpp"
#include "midi/xg_ram.hpp"
#include "test_support.hpp"

using namespace hvyxg;

namespace {

std::optional<test::BankFiles> mu_files() {
    auto f = test::asset_pair("HVYXG_MU_BIN", "tbl/mu1000/SXGMU1KX.TBL", "HVYXG_MU_DATA", "tbl/mu1000/SXGMU1KX.UPCM");
    if (!f) MESSAGE("HVYXG_MU_BIN / HVYXG_MU_DATA not set: skipped");
    return f;
}

std::shared_ptr<const Bank> mu_bank(MuVoiceMap map = MuVoiceMap::Default) {
    static std::shared_ptr<const Bank> cache[3];
    auto& b = cache[static_cast<int>(map)];
    if (b) return b;
    const auto f = mu_files();
    if (!f) return nullptr;
    b = load_bank(f->bin, f->data, Model::Auto, test::bank_options(BankOptions{.tables = {}, .enable_mu = true, .mu_voice_map = map}));
    return b;
}

/// One note on part 1 (or a drum note on part 10) after XG System On; left channel RMS in dB.
double note_db(std::shared_ptr<const Bank> b, std::uint8_t msb, std::uint8_t lsb, std::uint8_t prog,
               std::uint8_t key, std::uint8_t ch = 0) {
    test::Rig r(std::move(b));
    r.cc(ch, 0, msb);
    r.cc(ch, 32, lsb);
    r.program(ch, prog);
    r.note_on(ch, key, 100);
    return test::rms_db(r.render_left(0.5));
}

std::vector<float> render_with(Synth::Options o, const std::vector<std::vector<std::uint8_t>>& msgs) {
    Synth s(std::move(o));
    for (const auto& m : msgs) s.midi(m);
    std::vector<float> buf(2 * 22050);
    s.render(buf.data(), 22050);
    return buf;
}

} // namespace

TEST_CASE("mu: nearest-type map of the patched DLL") {
    using fx::Block;
    CHECK(fx::mu_effect_type(Block::Reverb, 0x12, 0x00) == std::pair<std::uint8_t, std::uint8_t>{0x11, 0x00});
    CHECK(fx::mu_effect_type(Block::Reverb, 0x01, 0x00) == std::pair<std::uint8_t, std::uint8_t>{0x01, 0x00});
    CHECK(fx::mu_effect_type(Block::Chorus, 0x44, 0x01) == std::pair<std::uint8_t, std::uint8_t>{0x42, 0x00});
    CHECK(fx::mu_effect_type(Block::Chorus, 0x48, 0x00) == std::pair<std::uint8_t, std::uint8_t>{0x43, 0x00});
    CHECK(fx::mu_effect_type(Block::Variation, 0x4E, 0x00) == std::pair<std::uint8_t, std::uint8_t>{0x4E, 0x00});
    CHECK(fx::mu_effect_type(Block::Variation, 0x4E, 0x02) == std::pair<std::uint8_t, std::uint8_t>{0x4A, 0x00});
    CHECK(fx::mu_effect_type(Block::Variation, 0x52, 0x01) == std::pair<std::uint8_t, std::uint8_t>{0x49, 0x00});
    CHECK(fx::mu_effect_type(Block::Variation, 0x52, 0x05) == std::pair<std::uint8_t, std::uint8_t>{0x4E, 0x00});
    CHECK(fx::mu_effect_type(Block::Variation, 0x5F, 0x00) == std::pair<std::uint8_t, std::uint8_t>{0x49, 0x00});
    CHECK(fx::mu_effect_type(Block::Variation, 0x61, 0x01) == std::pair<std::uint8_t, std::uint8_t>{0x4A, 0x00});
    // Types without a substitute stay (Thru in the DLL).
    CHECK(fx::mu_effect_type(Block::Variation, 0x50, 0x00) == std::pair<std::uint8_t, std::uint8_t>{0x50, 0x00});
}

TEST_CASE("mu: per-type return levels") {
    using fx::Block;
    CHECK(fx::mu_return_level(Block::Reverb, 0x01, 0x00, 64) == 85);   // Hall ×85/64
    CHECK(fx::mu_return_level(Block::Reverb, 0x02, 0x00, 64) == 96);   // Room 1
    CHECK(fx::mu_return_level(Block::Reverb, 0x02, 0x01, 64) == 81);   // Room 2
    CHECK(fx::mu_return_level(Block::Reverb, 0x01, 0x00, 127) == 127); // capped
    CHECK(fx::mu_return_level(Block::Reverb, 0x05, 0x00, 64) == 64);   // delays only on the variation
    CHECK(fx::mu_return_level(Block::Variation, 0x05, 0x00, 64) == 72);
    CHECK(fx::mu_return_level(Block::Variation, 0x14, 0x01, 64) == 48);
    CHECK(fx::mu_return_level(Block::Chorus, 0x41, 0x00, 64) == 64);
}

TEST_CASE("mu: a type write stores the substitute and loads its defaults") {
    midi::xg::EffectData plain{test::tables().get()}, mu{test::tables().get()};
    mu.mu_types = true;
    plain.write(0x20, 0x42);  // Celeste 1
    plain.write(0x21, 0x00);
    mu.write(0x20, 0x44);     // Symphonic → Celeste 1
    mu.write(0x21, 0x00);
    CHECK(mu[0x20] == 0x42);
    CHECK(mu[0x21] == 0x00);
    for (std::uint8_t a = 0x22; a <= 0x35; ++a) CHECK(mu[a] == plain[a]);
}

TEST_CASE("mu: a big-layout table needs enable_mu") {
    const auto f = mu_files();
    if (!f) return;
    CHECK_THROWS_AS(load_bank(f->bin, f->data, hvyxg::Model::Auto, test::bank_options()), BankError);
    CHECK_NOTHROW(load_bank(f->bin, f->data, Model::Auto, test::bank_options(BankOptions{.tables = {}, .enable_mu = true})));
    CHECK(mu_bank()->mu());
}

TEST_CASE("mu: voices, drums, MSB 48 and the voice maps play") {
    HVYXG_NEED_TABLES();
    const auto b = mu_bank();
    if (!b) return;
    // Melodic voices across the three multisample pages (GM piano, strings, SFX bank).
    CHECK(note_db(b, 0, 0, 0, 60) > -50);
    CHECK(note_db(b, 0, 0, 48, 60) > -50);
    CHECK(note_db(b, 64, 0, 0, 60) > -60);
    // Drum kit on part 10: kick and snare (own waves) and an SFX kit key (ext voice).
    CHECK(note_db(b, 127, 0, 0, 36, 9) > -50);
    CHECK(note_db(b, 127, 0, 0, 38, 9) > -50);
    double sfx = -300;
    for (std::uint8_t k = 36; k <= 84 && sfx < -60; k += 4) sfx = note_db(b, 126, 0, 0, k, 9);
    CHECK(sfx > -60);
    // MSB 48 with an LSB (the MSB 48 row of the appendix).
    CHECK(note_db(b, 48, 8, 0, 60) > -70);
    // The MU Basic map plays something, and differs from the native map on some voice.
    const auto basic = mu_bank(MuVoiceMap::Basic);
    REQUIRE(basic);
    CHECK(note_db(basic, 0, 0, 0, 60) > -50);
    bool differs = false;
    for (std::uint8_t p = 0; p < 128 && !differs; p += 7)
        differs = std::abs(note_db(b, 0, 0, p, 60) - note_db(basic, 0, 0, p, 60)) > 0.01;
    CHECK(differs);
}

TEST_CASE("dll-exact: superset-only messages are ignored") {
    HVYXG_NEED_TABLES();
    const auto f = test::bank_files(true);
    if (!f) {
        MESSAGE("HVYXG_TBL_BIN / HVYXG_TBL_DATA not set: skipped");
        return;
    }
    const std::vector<std::vector<std::uint8_t>> base{
        {0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7}, {0xC0, 81}, {0x90, 60, 100}};
    auto with = base;
    // Insertion block 1 on part 1 (distortion), multi EQ band 1 +12 dB, part HPF.
    with.insert(with.begin() + 1, {0xF0, 0x43, 0x10, 0x4C, 0x03, 0x00, 0x00, 0x49, 0x00, 0xF7});
    with.insert(with.begin() + 2, {0xF0, 0x43, 0x10, 0x4C, 0x03, 0x00, 0x0C, 0x00, 0xF7});
    with.insert(with.begin() + 3, {0xF0, 0x43, 0x10, 0x4C, 0x02, 0x40, 0x01, 0x4C, 0xF7});
    with.insert(with.begin() + 4, {0xF0, 0x43, 0x10, 0x4C, 0x0A, 0x00, 0x20, 0x7F, 0xF7});
    with.insert(with.begin() + 5, {0xF0, 0x43, 0x10, 0x4C, 0x08, 0x00, 0x72, 0x4C, 0xF7});
    Synth::Options o;
    o.tables = test::tables();
    o.bin_file = f->bin;
    o.data_file = f->data;
    // Superset: the messages act.
    CHECK(render_with(o, base) != render_with(o, with));
    o.dll_exact = true;
    CHECK(render_with(o, base) == render_with(o, with));
    // DLL-exact is the default output when no superset message is sent.
    Synth::Options d = o;
    d.dll_exact = false;
    CHECK(render_with(o, base) == render_with(d, base));
}

TEST_CASE("mu: the patched DLL's second engine takes notes once A is full") {
    HVYXG_NEED_TABLES();
    // SXG-Create-NF's polyphony patch (ENGINE.md "MU banks"): a note-on goes to engine B when
    // A has fewer than two free voices. A held marker chord (panned left) must survive 150 more
    // two-element string notes (panned right) with the second engine; with a single 128-voice
    // engine (an explicit polyphony, no dll_exact) a steal takes part of it.
    const auto b = mu_bank();
    if (!b) return;
    const auto marker_db = [&](bool single, bool marker = true, std::uint16_t poly = 128) {
        Synth::Options o;
        o.tables = test::tables();
        o.bank = b;
        if (single) o.max_polyphony = poly;
        test::Rig r(o);
        for (std::uint8_t ch = 0; ch < 16; ++ch) {
            if (ch == 9) continue;
            r.program(ch, 48);
            r.cc(ch, 91, 0);
            r.cc(ch, 93, 0);
            r.cc(ch, 10, ch == 15 ? 1 : 127);
        }
        // The last part (the first a steal looks at), more voices than its element reserve.
        if (marker)
            for (std::uint8_t k : {60, 64, 67, 72}) r.note_on(15, k, 100);
        r.render(4410);
        int n = 0;
        for (std::uint8_t k = 0; k < 11 && n < 150; ++k)
            for (std::uint8_t ch = 0; ch < 15 && n < 150; ++ch, ++n) {
                if (ch == 9) continue;
                r.note_on(ch, static_cast<std::uint8_t>(40 + 3 * k + ch), 100);
                r.render(256);
            }
        r.render(22050);
        const auto st = r.render(22050);
        double e = 0;
        for (std::size_t i = 0; i < st.size(); i += 2) e += double(st[i]) * st[i];
        return 10 * std::log10(e / double(st.size() / 2) + 1e-30);
    };
    const double two = marker_db(false), one = marker_db(true);
    CHECK(marker_db(false, false) < -200);  // only the marker sounds on the left
    CAPTURE(two);
    CAPTURE(one);
    // Stolen in part with one engine; whole with two, as with one 256-voice engine (no steal).
    CHECK(two > one + 2);
    CHECK(two == doctest::Approx(marker_db(true, true, 256)).epsilon(0.02));
}
