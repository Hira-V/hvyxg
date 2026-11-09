// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "midi/gs_effects.hpp"

#include "engine_tables.hpp"

#include <algorithm>
#include <array>

namespace hvyxg::midi::gs {

namespace {

// The GS layer's tables come from the DLLs, in the engine tables the effect RAM carries
// (EffectData::tables; docs/ENGINE.md "GS mode").
const fx::Tables& fxt(const xg::EffectData& r) { return r.tables->fx; }
const syxg50::dll::Tables& s50(const xg::EffectData& r) { return r.tables->syxg50; }
const syxg2006le::tbl::dll::Tables& le(const xg::EffectData& r) { return r.tables->le; }

// XG effect RAM addresses (02 01 xx).
constexpr std::uint8_t kReverbType = 0x00;
constexpr std::uint8_t kReverbReturn = 0x0C;
constexpr std::uint8_t kChorusType = 0x20;
constexpr std::uint8_t kChorusReturn = 0x2C;
constexpr std::uint8_t kChorusToReverb = 0x2E;
constexpr std::uint8_t kGsChorusMsb = 0x41;

/// XG reverb parameter n (1..16) address.
constexpr std::uint8_t reverb_param(int n) {
    return static_cast<std::uint8_t>(n <= 10 ? 0x02 + (n - 1) : 0x10 + (n - 11));
}
/// XG chorus parameter n (1..16) address.
constexpr std::uint8_t chorus_param(int n) {
    return static_cast<std::uint8_t>(n <= 10 ? 0x22 + (n - 1) : 0x30 + (n - 11));
}

std::uint8_t clamp7(std::uint8_t v) { return std::min<std::uint8_t>(v, 7); }

// GS chorus value → XG Chorus 1 parameter (both DLLs; syxg50.dll computes them, the 2006LE
// reads the same values from tables).
std::uint8_t chorus_rate(std::uint8_t v) { return v >= 48 ? 127 : static_cast<std::uint8_t>(v * 8 / 3); }
std::uint8_t chorus_depth(std::uint8_t v) { return v >= 96 ? 127 : static_cast<std::uint8_t>(v * 4 / 3); }
std::uint8_t chorus_feedback(std::uint8_t v) { return static_cast<std::uint8_t>(64 + v / 2); }

// ── S-YXG50 ─────────────────────────────────────────────────────────────────────────────

void s50_apply_character(const EffectState& gs, xg::EffectData& ram) {
    const auto& type = s50(ram).kGsCharacterType[gs.reverb_character];
    ram.write(kReverbType, type[0]);
    ram.write(kReverbType + 1, type[1]);
    const auto& row = s50(ram).kGsCharacterParams[gs.reverb_character];
    for (int n = 1; n <= 16; ++n) ram.write(reverb_param(n), row[static_cast<std::size_t>(n - 1)]);
    ram.write(reverb_param(1), reverb_time(*ram.tables, gs.reverb_character, gs.reverb_time));
    ram.write(reverb_param(5), fxt(ram).kGsPreLpf[gs.reverb_pre_lpf]);
}

void s50_write(EffectState& gs, xg::EffectData& ram, std::uint8_t addr, std::uint8_t v) {
    // The character, pre-LPF and time apply while the reverb is a GS-like type (MSB ≤ 4),
    // the chorus parameters while it is an XG Chorus (MSB 41) [code].
    const bool reverb_gs = ram[kReverbType] <= 4;
    const bool chorus_gs = ram[kChorusType] == kGsChorusMsb;
    switch (addr) {
    case 0x30: {
        gs.reverb_macro = clamp7(v);
        const auto& m = s50(ram).kGsReverbMacro[gs.reverb_macro];
        gs.reverb_character = m[0];
        gs.reverb_pre_lpf = m[1];
        gs.reverb_level = m[2];
        gs.reverb_time = m[3];
        gs.reverb_feedback = m[4];
        gs.reverb_predelay = m[5];
        s50_apply_character(gs, ram);
        ram.write(kReverbReturn, gs.reverb_level);
        break;
    }
    case 0x31:
        gs.reverb_character = clamp7(v);
        if (reverb_gs) s50_apply_character(gs, ram);
        break;
    case 0x32:
        gs.reverb_pre_lpf = clamp7(v);
        if (reverb_gs) ram.write(reverb_param(5), fxt(ram).kGsPreLpf[gs.reverb_pre_lpf]);
        break;
    case 0x33:
        gs.reverb_level = v;
        ram.write(kReverbReturn, v);
        break;
    case 0x34:
        gs.reverb_time = v;
        if (reverb_gs) ram.write(reverb_param(1), reverb_time(*ram.tables, gs.reverb_character, v));
        break;
    case 0x35: gs.reverb_feedback = v; break;
    case 0x37: gs.reverb_predelay = v; break;
    case 0x38: {
        gs.chorus_macro = clamp7(v);
        const auto& m = s50(ram).kGsChorusMacro[gs.chorus_macro];
        gs.chorus_pre_lpf = m[0];
        gs.chorus_level = m[1];
        gs.chorus_feedback = m[2];
        gs.chorus_delay = m[3];
        gs.chorus_rate = std::min<std::uint8_t>(m[4], 47);
        gs.chorus_depth = std::min<std::uint8_t>(m[5], 96);
        gs.chorus_send_reverb = m[6];
        ram.write(kChorusType, kGsChorusMsb);
        ram.write(kChorusType + 1, 0x00);
        for (int n = 1; n <= 16; ++n) ram.write(chorus_param(n), s50(ram).kGsChorusParams[static_cast<std::size_t>(n - 1)]);
        ram.write(chorus_param(1), chorus_rate(m[4]));
        ram.write(chorus_param(2), chorus_depth(m[5]));
        ram.write(chorus_param(3), chorus_feedback(m[2]));
        ram.write(chorus_param(4), m[3]);
        ram.write(kChorusToReverb, gs.chorus_send_reverb);
        ram.write(kChorusReturn, gs.chorus_level);
        break;
    }
    case 0x39: gs.chorus_pre_lpf = clamp7(v); break;
    case 0x3A:
        gs.chorus_level = v;
        if (chorus_gs) ram.write(kChorusReturn, v);
        break;
    case 0x3B:
        gs.chorus_feedback = v;
        if (chorus_gs) ram.write(chorus_param(3), chorus_feedback(v));
        break;
    case 0x3C:
        gs.chorus_delay = v;
        if (chorus_gs) ram.write(chorus_param(4), v);
        break;
    case 0x3D:
        gs.chorus_rate = std::min<std::uint8_t>(v, 47);
        if (chorus_gs) ram.write(chorus_param(1), chorus_rate(v));
        break;
    case 0x3E:
        gs.chorus_depth = std::min<std::uint8_t>(v, 96);
        if (chorus_gs) ram.write(chorus_param(2), chorus_depth(v));
        break;
    case 0x3F:
        gs.chorus_send_reverb = v;
        if (chorus_gs) ram.write(kChorusToReverb, v);
        break;
    case 0x40: gs.chorus_send_delay = v; break;
    default: break;
    }
}

// ── S-YXG2006LE ─────────────────────────────────────────────────────────────────────────

/// The XG messages of a reverb macro (row = macro) or character (row = character): the GS time
/// table follows `time_row` (the macro for both, the character for a character write).
void le_send_reverb(const EffectState& gs, xg::EffectData& ram, std::uint8_t row_index,
                    std::uint8_t time_row) {
    const auto& row = le(ram).kGsReverb[row_index];
    ram.write(kReverbType, row[0]);
    ram.write(kReverbType + 1, row[1]);
    ram.write(reverb_param(1), reverb_time(*ram.tables, time_row, gs.reverb_time));
    for (int n = 2; n <= 4; ++n) ram.write(reverb_param(n), row[static_cast<std::size_t>(n + 1)]);
    ram.write(reverb_param(5), fxt(ram).kGsPreLpf[gs.reverb_pre_lpf]);
    ram.write(kReverbReturn, gs.reverb_level);
    for (int n = 11; n <= 15; ++n) ram.write(reverb_param(n), row[static_cast<std::size_t>(n + 1)]);
}

void le_chorus_macro(xg::EffectData& ram, std::uint8_t m) {
    const auto& row = le(ram).kGsChorus[m];
    ram.write(kChorusType, row[0]);
    ram.write(kChorusType + 1, row[1]);
    for (int n = 1; n <= 4; ++n) ram.write(chorus_param(n), row[static_cast<std::size_t>(n + 1)]);
    ram.write(kChorusReturn, row[18]);
    ram.write(chorus_param(11), row[12]);
}

void le_write(EffectState& gs, xg::EffectData& ram, std::uint8_t addr, std::uint8_t v) {
    switch (addr) {
    case 0x30: {
        gs.reverb_macro = clamp7(v);
        const auto& row = le(ram).kGsReverb[gs.reverb_macro];
        gs.reverb_pre_lpf = row[6];
        gs.reverb_level = row[18];
        gs.reverb_time = row[2];
        le_send_reverb(gs, ram, gs.reverb_macro, gs.reverb_macro);
        break;
    }
    case 0x31:
        // The character row with the current GS pre-LPF, level and time; not stored.
        gs.reverb_character = clamp7(v);
        le_send_reverb(gs, ram, gs.reverb_character, gs.reverb_character);
        break;
    case 0x32:
        gs.reverb_pre_lpf = clamp7(v);
        ram.write(reverb_param(5), fxt(ram).kGsPreLpf[gs.reverb_pre_lpf]);
        break;
    case 0x33:
        gs.reverb_level = v;
        ram.write(kReverbReturn, v);
        break;
    case 0x34:
        gs.reverb_time = v;
        ram.write(reverb_param(1), reverb_time(*ram.tables, gs.reverb_macro, v));
        break;
    case 0x38:
        gs.chorus_macro = clamp7(v);
        le_chorus_macro(ram, gs.chorus_macro);
        break;
    case 0x3A: gs.chorus_level = v; ram.write(kChorusReturn, v); break;
    case 0x3B: gs.chorus_feedback = v; ram.write(chorus_param(3), chorus_feedback(v)); break;
    case 0x3C: gs.chorus_delay = v; ram.write(chorus_param(4), v); break;
    case 0x3D: gs.chorus_rate = v; ram.write(chorus_param(1), chorus_rate(v)); break;
    case 0x3E: gs.chorus_depth = v; ram.write(chorus_param(2), chorus_depth(v)); break;
    case 0x3F: gs.chorus_send_reverb = v; ram.write(kChorusToReverb, v); break;
    default: break;  // 35..37, 39, 40: no handler
    }
}

} // namespace

std::uint8_t reverb_time(const EngineTables& tables, std::uint8_t character, std::uint8_t time) {
    time &= 0x7F;
    return (character == 0 || character == 5) ? tables.fx.kGsTimeRoom1Plate[time] : tables.fx.kGsTimeOther[time];
}

void reset(EffectState& gs, xg::EffectData& ram, Dialect d) {
    gs = EffectState{};
    if (!ram.tables) return;  // no engine tables: nothing to translate with
    if (d == Dialect::Syxg50) {
        // The reverb type becomes Hall 2 over the XG reset image's Hall 1 parameters (the
        // effect control gets algorithm 2 with the same 16 parameters); chorus and its parameters
        // keep the XG image [meas: register log].
        std::array<std::uint8_t, 16> keep{};
        for (int n = 1; n <= 16; ++n) keep[static_cast<std::size_t>(n - 1)] = ram[reverb_param(n)];
        ram.write(kReverbType, 0x01);
        ram.write(kReverbType + 1, 0x01);
        for (int n = 1; n <= 16; ++n) ram.write(reverb_param(n), keep[static_cast<std::size_t>(n - 1)]);
        return;
    }
    // The 2006LE's GS reset sends the Hall 2 macro and a Chorus 3 macro whose delay is 0x05
    // instead of the macro's 0x50 [code].
    le_send_reverb(gs, ram, gs.reverb_macro, gs.reverb_macro);
    le_chorus_macro(ram, gs.chorus_macro);
    ram.write(chorus_param(4), 0x05);
}

void write(EffectState& gs, xg::EffectData& ram, Dialect d, std::uint8_t addr, std::uint8_t value,
           bool single) {
    if (!ram.tables) return;
    value &= 0x7F;
    if (d == Dialect::Syxg50) {
        s50_write(gs, ram, addr, value);
    } else if (single) {
        le_write(gs, ram, addr, value);
    }
}

} // namespace hvyxg::midi::gs
