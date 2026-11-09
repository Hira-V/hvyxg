// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "midi/xg_ram.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>

#include "fx/fx_control.hpp"
#include "fx/fx_insertion.hpp"
#include "fx/fx_mu.hpp"
#include "engine_tables.hpp"
#include "fx/fx_tables.hpp"

namespace hvyxg::midi::xg {

namespace {

std::uint8_t clamp(std::uint8_t v, int lo, int hi) {
    return static_cast<std::uint8_t>(std::clamp<int>(v, lo, hi));
}

/// Range of a multi-part parameter; {−1, −1} = not a parameter.
struct Range {
    int lo, hi;
};

Range part_range(std::uint8_t a) {
    // XG specification, MULTI PART table (MU100 / MU2000 owner's manuals).
    if (a == 0x00) return {0, 32};
    if (a == 0x04) return {0, 127};  // 0..63 = port·16 + channel, 0x7F off (checked below)
    if (a == 0x05) return {0, 1};
    if (a == 0x06) return {0, 2};
    if (a == 0x07) return {0, static_cast<int>(1 + kDrumSetups)};
    if (a == 0x08) return {0x28, 0x58};
    if (a == 0x09 || a == 0x0A) return {0, 15};
    if (a <= 0x1C) return {0, 127};
    if (a == 0x1D || a == 0x23 || a == 0x4D || a == 0x53 || a == 0x5A || a == 0x61) return {0x28, 0x58};
    if (a <= 0x28) return {0, 127};
    if (a < 0x30) return {-1, -1};
    if (a <= 0x40) return {0, 1};
    if (a <= 0x58) return {0, 127};
    if (a == 0x59 || a == 0x60) return {0, 95};
    if (a <= 0x66) return {0, 127};
    if (a == 0x67) return {0, 1};
    if (a <= 0x6E) return {0, 127};
    if (a == 0x71) return {0, 127};
    if (a == 0x72 || a == 0x73) return {0x34, 0x4C};
    if (a == 0x76) return {4, 40};
    if (a == 0x77) return {28, 58};
    return {-1, -1};
}

} // namespace

// ── System ──────────────────────────────────────────────────────────────────────────────

bool System::write(std::uint8_t addr, std::uint8_t v) {
    switch (addr) {
    case 0x00: master_tune1 = v & 15; return true;
    case 0x01: master_tune2 = v & 15; return true;
    case 0x02: master_tune3 = v & 15; return true;
    case 0x03: master_tune4 = v & 15; return true;
    case 0x04: master_volume = clamp(v, 0, 127); return true;
    case 0x05: master_attenuator = clamp(v, 0, 127); return true;
    case 0x06: transpose = clamp(v, 0x28, 0x58); return true;
    default: return false;
    }
}

// ── Multi part ──────────────────────────────────────────────────────────────────────────

void MultiPart::reset(std::size_t part) {
    std::memset(this, 0, sizeof *this);
    const bool drum = part % 16 == 9;
    element_reserve = drum ? 0 : 2;
    bank_msb = drum ? 127 : 0;
    bank_lsb = 0;
    program_number = 0;
    rcv_channel = static_cast<std::uint8_t>(part < 64 ? part : 0x7F);
    mode = 1;
    key_assign = 1;  // the drum part's per-engine default is applied by the front end
    part_mode = drum ? 2 : 0;
    note_shift = 64;
    detune_msb = 0x08;
    detune_lsb = 0x00;
    volume = 100;
    velocity_sense_depth = 64;
    velocity_sense_offset = 64;
    pan = 64;
    note_limit_low = 0;
    note_limit_high = 127;
    dry_level = 127;
    chorus_send = 0;
    reverb_send = 40;
    variation_send = 0;
    vibrato_rate = vibrato_depth = vibrato_delay = 64;
    filter_cutoff_freq = filter_resonance = 64;
    eg_attack_time = eg_decay_time = eg_release_time = 64;
    // MW: pitch / filter / amp centred, LFO PMod 10 (≈ ±50 cents at full wheel).
    mw = {64, 64, 64, 10, 0, 0};
    // Bend: range +2 semitones; the bend LFO depths are 0 (measured: syxg50 part template
    // holds 42 40 40 00 00 00).
    bend = {0x42, 64, 64, 0, 0, 0};
    std::fill(&rcv_pitch_bend, &rcv_bank_select + 1, std::uint8_t{1});
    scale_tuning.fill(64);
    cat = {64, 64, 64, 0, 0, 0};
    pat = {64, 64, 64, 0, 0, 0};
    // AC1 = CC16, AC2 = CC17 (measured; the XG defaults).
    ac[0] = {16, 64, 64, 64, 0, 0, 0};
    ac[1] = {17, 64, 64, 64, 0, 0, 0};
    portamento_switch = 0;
    portamento_time = 0;
    pitch_eg_init_level = pitch_eg_attack_time = pitch_eg_release_level = pitch_eg_release_time = 64;
    velocity_limit_low = 1;
    velocity_limit_high = 127;
    filter_eg_depth = 64;
    eq_bass_gain = eq_treble_gain = 64;
    eq_bass_freq = 0x0C;
    eq_treble_freq = 0x36;
}

bool MultiPart::write(std::uint8_t addr, std::uint8_t value) {
    addr &= 0x7F;
    const Range r = part_range(addr);
    if (r.lo < 0) return false;
    std::uint8_t v = clamp(value, r.lo, r.hi);
    if (addr == 0x04 && v > 63) v = 0x7F;
    bytes()[addr] = v;
    return true;
}

// ── Drum setup ──────────────────────────────────────────────────────────────────────────

DrumSetup DrumSetup::from_bytes(std::span<const std::uint8_t> b) {
    DrumSetup d;
    std::memcpy(&d, b.data(), std::min(b.size(), kBytes));
    return d;
}

bool DrumSetup::is_empty() const {
    const auto* p = reinterpret_cast<const std::uint8_t*>(this);
    return std::all_of(p, p + kBytes, [](std::uint8_t x) { return x == 0; });
}

int DrumSetup::index_of(std::uint8_t addr) {
    if (addr < 16) return addr;
    switch (addr) {
    case 0x20: return 16;
    case 0x21: return 17;
    case 0x24: return 18;
    case 0x25: return 19;
    case 0x50: return 21;
    case 0x60: return 22;
    case 0x61: return 23;
    default: return -1;
    }
}

bool DrumSetup::write(std::uint8_t addr, std::uint8_t value) {
    const int i = index_of(addr);
    if (i < 0) return false;
    int lo = 0, hi = 127;
    switch (i) {
    case 8: case 9: case 10: hi = 1; break;
    case 16: case 17: lo = 0x34; hi = 0x4C; break;
    case 18: lo = 4; hi = 40; break;
    case 19: lo = 28; hi = 58; break;
    default: break;
    }
    reinterpret_cast<std::uint8_t*>(this)[i] = clamp(value, lo, hi);
    return true;
}

// ── Effect ──────────────────────────────────────────────────────────────────────────────

void EffectData::reset() {
    // The XG reset state: the default types (reverb Hall 1, chorus Chorus 1, variation Delay LCR,
    // system connection) with their defaults, returns and pans at the centre (0x40), part off
    // (0x7F) and the controller depths at 0x40 (XG specification, effect parameter defaults).
    // It is the S-YXG2006LE's reset image byte for byte (tests/fx_variation_test.cpp checks its hash).
    bytes.fill(0);
    bytes[0x00] = 0x01;
    bytes[0x20] = 0x41;
    bytes[0x40] = 0x05;
    for (const std::uint8_t a : {0x0C, 0x0D, 0x2C, 0x2D, 0x56, 0x57, 0x5C, 0x5D, 0x5E, 0x5F, 0x60}) bytes[a] = 0x40;
    bytes[0x5B] = 0x7F;
    load_reverb_defaults();
    load_chorus_defaults();
    load_variation_defaults();
    // Seven parameters reset to other values than their type's defaults: reverb 10 and 12 / 13,
    // chorus 10 and 11 / 13, variation 6 (Delay L,C,R: C level 127).
    for (const auto& [a, v] : {std::pair<std::uint8_t, std::uint8_t>{0x0B, 127}, {0x11, 4}, {0x12, 52}, {0x2B, 127},
                              {0x30, 48}, {0x32, 20}, {0x4D, 127}})
        bytes[a] = v;
    restart();
}

void EffectData::restart() {
    ++restarts;
    ++reverb_type_writes;
    ++chorus_type_writes;
    ++variation_type_writes;
    ++variation_connection_writes;
}

void EffectData::load_reverb_defaults() {
    if (!tables) return;
    const fx::Tables& tb = tables->fx;
    const auto p = fx::reverb_defaults(tb, fx::reverb_algorithm(tb, bytes[0x00], bytes[0x01]));
    for (std::size_t i = 0; i < 10; ++i) bytes[0x02 + i] = static_cast<std::uint8_t>(std::min<std::uint16_t>(p[i], 127));
    for (std::size_t i = 0; i < 6; ++i) bytes[0x10 + i] = static_cast<std::uint8_t>(std::min<std::uint16_t>(p[10 + i], 127));
}

void EffectData::load_chorus_defaults() {
    if (!tables) return;
    const fx::Tables& tb = tables->fx;
    const auto p = fx::chorus_defaults(tb, fx::chorus_algorithm(tb, bytes[0x20], bytes[0x21]));
    for (std::size_t i = 0; i < 10; ++i) bytes[0x22 + i] = static_cast<std::uint8_t>(std::min<std::uint16_t>(p[i], 127));
    for (std::size_t i = 0; i < 6; ++i) bytes[0x30 + i] = static_cast<std::uint8_t>(std::min<std::uint16_t>(p[10 + i], 127));
}

void EffectData::load_variation_defaults() {
    if (!tables) return;
    const fx::Tables& tb = tables->fx;
    const auto p = fx::variation_defaults(tb, fx::variation_type(tb, bytes[0x40], bytes[0x41], bytes[0x5A] == 1));
    for (std::size_t i = 0; i < 10; ++i) {
        bytes[0x42 + 2 * i] = static_cast<std::uint8_t>((p[i] >> 7) & 0x7F);
        bytes[0x43 + 2 * i] = static_cast<std::uint8_t>(p[i] & 0x7F);
    }
    for (std::size_t i = 0; i < 6; ++i) bytes[0x70 + i] = static_cast<std::uint8_t>(std::min<std::uint16_t>(p[10 + i], 127));
}

bool EffectData::write(std::uint8_t addr, std::uint8_t value) {
    addr &= 0x7F;
    const bool used = addr <= 0x15 || (addr >= 0x20 && addr <= 0x35) || (addr >= 0x40 && addr <= 0x60) ||
                      (addr >= 0x70 && addr <= 0x75);
    if (!used) return false;
    std::uint8_t v = value & 0x7F;
    if (addr == 0x5A) {
        v = std::min<std::uint8_t>(v, 1);
        ++variation_connection_writes;
    }
    bytes[addr] = v;
    if (mu_types) {
        const auto sub = [&](std::uint8_t base, fx::Block block) {
            const auto [m, l] = fx::mu_effect_type(block, bytes[base], bytes[base + 1]);
            bytes[base] = m;
            bytes[base + 1] = l;
        };
        if (addr <= 0x01) sub(0x00, fx::Block::Reverb);
        else if (addr == 0x20 || addr == 0x21) sub(0x20, fx::Block::Chorus);
        else if (addr == 0x40 || addr == 0x41) sub(0x40, fx::Block::Variation);
    }
    // A type write (MSB or LSB, also the same value) loads the type's defaults (2006LE: any
    // single-byte type write; the effects count it).
    if (addr <= 0x01) {
        load_reverb_defaults();
        ++reverb_type_writes;
    } else if (addr == 0x20 || addr == 0x21) {
        load_chorus_defaults();
        ++chorus_type_writes;
    } else if (addr == 0x40 || addr == 0x41) {
        load_variation_defaults();
        ++variation_type_writes;
    }
    return true;
}

// ── Insertion ───────────────────────────────────────────────────────────────────────────

bool EffectInsertion::requires_msb(std::uint8_t msb) {
    switch (msb) {
    case 0x05: case 0x06: case 0x07: case 0x08:          // delays, cross delay
    case 0x5F: case 0x60: case 0x61: case 0x62:          // +DELAY combinations, V-distortion
    case 0x68: case 0x76:
        return true;
    default: return false;
    }
}

void EffectInsertion::load_defaults() {
    const std::uint8_t msb = bytes[0x00], lsb = bytes[0x01];
    std::array<std::uint16_t, 16> p{};
    if (const auto own = fx::xg20_insertion_defaults(msb, lsb)) p = *own;
    else if (tables) p = fx::variation_defaults(tables->fx, fx::variation_type(tables->fx, msb, lsb, false));
    if (requires_msb(msb)) {
        for (std::size_t i = 0; i < 10; ++i) {
            bytes[0x30 + 2 * i] = static_cast<std::uint8_t>((p[i] >> 7) & 0x7F);
            bytes[0x31 + 2 * i] = static_cast<std::uint8_t>(p[i] & 0x7F);
        }
    } else {
        for (std::size_t i = 0; i < 10; ++i) bytes[0x02 + i] = static_cast<std::uint8_t>(std::min<std::uint16_t>(p[i], 127));
    }
    for (std::size_t i = 0; i < 6; ++i) bytes[0x20 + i] = static_cast<std::uint8_t>(std::min<std::uint16_t>(p[10 + i], 127));
}

void EffectInsertion::reset() {
    bytes.fill(0);
    bytes[0x00] = 0x49;  // Distortion
    bytes[0x01] = 0x00;
    bytes[0x0C] = 0x7F;  // part off
    for (std::size_t a = 0x0D; a <= 0x11; ++a) bytes[a] = 64;  // MW / bend / CAT / AC1 / AC2 depth
    load_defaults();
    ++type_writes;
}

bool EffectInsertion::write(std::uint8_t addr, std::uint8_t value) {
    addr &= 0x7F;
    const bool used = addr <= 0x11 || (addr >= 0x20 && addr <= 0x25) || (addr >= 0x30 && addr <= 0x43);
    if (!used) return false;
    std::uint8_t v = value & 0x7F;
    if (addr == 0x0C && v > 63) v = 0x7F;
    bytes[addr] = v;
    if (addr <= 0x01) {
        load_defaults();
        ++type_writes;
    }
    return true;
}

std::uint16_t EffectInsertion::param(std::size_t i) const {
    if (i >= 10) return bytes[0x20 + (i - 10)];
    if (requires_msb(bytes[0x00]))
        return static_cast<std::uint16_t>(bytes[0x30 + 2 * i] * 128 + (bytes[0x31 + 2 * i] & 0x7F));
    return bytes[0x02 + i];
}

// ── Multi EQ ────────────────────────────────────────────────────────────────────────────

// The MU2000's presets, measured: each type written (02 40 00) to the MU2000 firmware running in
// S-MU2000, and the five bands read back; every type also sets both shapes
// to shelf. Gain, frequency, Q per band.
const std::array<std::array<std::uint8_t, 15>, 5> MultiEq::kTypePresets = {{
    {0x40, 0x0C, 7, 0x40, 0x1C, 7, 0x40, 0x22, 7, 0x40, 0x2E, 7, 0x40, 0x34, 7},     // flat
    {0x3A, 0x08, 7, 0x42, 0x10, 3, 0x44, 0x21, 3, 0x3C, 0x2C, 5, 0x3A, 0x32, 7},     // jazz
    {0x44, 0x10, 7, 0x3C, 0x18, 20, 0x43, 0x22, 7, 0x3C, 0x28, 20, 0x46, 0x30, 7},   // pops
    {0x47, 0x10, 7, 0x44, 0x14, 7, 0x3C, 0x24, 5, 0x44, 0x29, 10, 0x42, 0x32, 7},    // rock
    {0x43, 0x0C, 7, 0x44, 0x18, 7, 0x40, 0x22, 5, 0x42, 0x32, 7, 0x3D, 0x34, 7},     // classic
}};

namespace {

void load_eq_preset(std::array<std::uint8_t, 0x15>& b, std::size_t type) {
    const auto& p = MultiEq::kTypePresets[type];
    for (std::size_t k = 0; k < MultiEq::kBands; ++k)
        for (std::size_t j = 0; j < 3; ++j) b[1 + 4 * k + j] = p[3 * k + j];
    b[0x04] = b[0x14] = 0;
}

} // namespace

void MultiEq::reset() {
    bytes.fill(0);
    load_eq_preset(bytes, 0);
}

bool MultiEq::write(std::uint8_t addr, std::uint8_t value) {
    if (addr >= bytes.size()) return false;
    if (addr == 0x00) {
        bytes[0] = clamp(value, 0, 4);
        load_eq_preset(bytes, bytes[0]);
        return true;
    }
    const std::size_t k = (addr - 1u) / 4, j = (addr - 1u) % 4;
    int lo = 0, hi = 0;
    switch (j) {
    case 0: lo = 0x34; hi = 0x4C; break;                                    // gain
    case 1: lo = k == 0 ? 4 : k == 4 ? 28 : 14; hi = k == 0 ? 40 : k == 4 ? 58 : 54; break;  // freq
    case 2: lo = 1; hi = 120; break;                                        // Q
    default:
        if (k != 0 && k != 4) return false;                                 // 08, 0C, 10 reserved
        hi = 1;                                                             // shape
        break;
    }
    bytes[addr] = clamp(value, lo, hi);
    return true;
}

MultiEq::Band MultiEq::band(std::size_t k) const {
    const std::size_t a = 1 + 4 * k;
    const bool peak = (k == 0 || k == 4) ? bytes[a + 3] != 0 : true;
    return {bytes[a], bytes[a + 1], bytes[a + 2], peak};
}

bool MultiEq::flat() const {
    for (std::size_t k = 0; k < kBands; ++k)
        if (bytes[1 + 4 * k] != 0x40) return false;
    return true;
}

// ── Multi part extension ────────────────────────────────────────────────────────────────

bool MultiPartExt::write(std::uint8_t addr, std::uint8_t value) {
    if (addr == 0x01) {
        mono_priority = clamp(value, 0, 1);
        return true;
    }
    if (addr >= 0x40 && addr <= 0x45) {
        level_control[addr - 0x40u] = clamp(value, 0, 127);
        return true;
    }
    if (addr != 0x20) return false;
    hpf_cutoff = clamp(value, 0, 127);
    return true;
}

} // namespace hvyxg::midi::xg
