// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The XG parameter memory: the blocks the XG specification addresses with
// `F0 43 1n 4C hh mm ll dd … F7`, laid out byte for byte in XG address order so that a
// SysEx write is a store at its address and the engine reads named fields.
//
// - `System`        00 00 00..06
// - `EffectData`    02 01 00..7F (reverb, chorus, variation)
// - `EffectInsertion` 03 nn 00..7F (XG2.0 insertion blocks)
// - `MultiEq`       02 40 00..14 (XG2.0 multi EQ)
// - `MultiPart`     08 nn 00..7F
// - `MultiPartExt`  0A nn 20 (XG2.0 part HPF)
// - `DrumSetup`     3n rr 00..61 (one note; stored in the S-YXG2006LE's 24-byte order)
//
// Defaults and ranges follow the Yamaha XG specification and the MU-series parameter tables
// unless a comment says the DLLs measure otherwise.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace hvyxg {
struct EngineTables;
}

namespace hvyxg::midi::xg {

/// Notes 13..91 of a drum setup.
inline constexpr std::size_t kDrumSetupNotes = 79;
inline constexpr std::uint8_t kDrumSetupFirstNote = 13;
/// Drum setup blocks 3n (XG: 2; the MU2000 has 4, part modes 2..5).
inline constexpr std::size_t kDrumSetups = 4;
/// Insertion blocks 03 nn (XG2.0 / MU2000: 4).
inline constexpr std::size_t kEffectInsertionSize = 4;

// ── System 00 00 xx ─────────────────────────────────────────────────────────────────────

struct System {
    /// Master tune: four nibbles of a 16-bit value, 0x0400 = 0 cents, 0.1 cent per step.
    std::uint8_t master_tune1 = 0;
    std::uint8_t master_tune2 = 4;
    std::uint8_t master_tune3 = 0;
    std::uint8_t master_tune4 = 0;
    std::uint8_t master_volume = 127;
    std::uint8_t master_attenuator = 0;
    /// Transpose, 0x40 = 0 (0x28..0x58 = −24..+24 semitones).
    std::uint8_t transpose = 64;
    /// Not XG memory: the GM2 master coarse tuning (universal real-time 04 04), 0x40 = 0
    /// semitones. Both DLLs apply it, like the master tune, to sounding notes [meas].
    std::uint8_t master_coarse = 64;
    /// Not XG memory: the GM2 master fine tuning (universal real-time 04 03) in cents, for
    /// engines that keep it apart from the XG master tune (`Bank::xg_parameter_table()`); the others fold it into the master tune.
    std::int16_t master_fine_gm2 = 0;
    /// The master tune the S-YXG50 takes at the note-on: the RAM's after each
    /// master tune it applies (not one whose end-of-message step was skipped). Other engines:
    /// the RAM.
    std::uint16_t applied_master_tune = 0x0400;

    std::uint16_t get_master_tune() const {
        return static_cast<std::uint16_t>((master_tune1 & 15) << 12 | (master_tune2 & 15) << 8 |
                                          (master_tune3 & 15) << 4 | (master_tune4 & 15));
    }
    void reset() { *this = System{}; }
    /// Store one byte (clamped to the parameter's range); false for an unknown address.
    bool write(std::uint8_t addr, std::uint8_t value);
};

// ── Multi part 08 nn xx ─────────────────────────────────────────────────────────────────

/// One controller's destinations (MW 1D, bend 23, CAT 4D, PAT 53).
struct ControlDepths {
    std::uint8_t pitch_control;
    std::uint8_t filter_control;
    std::uint8_t amplitude_control;
    std::uint8_t lfo_pmod_depth;
    std::uint8_t lfo_fmod_depth;
    std::uint8_t lfo_amod_depth;
};

/// An assignable controller (AC1 59, AC2 60): its control number, then its destinations.
struct AssignableControl {
    std::uint8_t controller_number;
    std::uint8_t pitch_control;
    std::uint8_t filter_control;
    std::uint8_t amplitude_control;
    std::uint8_t lfo_pmod_depth;
    std::uint8_t lfo_fmod_depth;
    std::uint8_t lfo_amod_depth;
};

struct MultiPart {
    std::uint8_t element_reserve;      // 00
    std::uint8_t bank_msb;             // 01
    std::uint8_t bank_lsb;             // 02
    std::uint8_t program_number;       // 03
    std::uint8_t rcv_channel;          // 04: 0..63 = port·16 + channel, 0x7F off
    std::uint8_t mode;                 // 05: 0 mono, 1 poly
    std::uint8_t key_assign;           // 06: 0 single, 1 multi, 2 inst (drums)
    std::uint8_t part_mode;            // 07: 0 normal, 1 drum, 2.. drum setup 1..
    std::uint8_t note_shift;           // 08
    std::uint8_t detune_msb;           // 09: bits 7-4 of the detune byte
    std::uint8_t detune_lsb;           // 0A: bits 3-0
    std::uint8_t volume;               // 0B
    std::uint8_t velocity_sense_depth; // 0C
    std::uint8_t velocity_sense_offset; // 0D
    std::uint8_t pan;                  // 0E: 0 = random
    std::uint8_t note_limit_low;       // 0F
    std::uint8_t note_limit_high;      // 10
    std::uint8_t dry_level;            // 11
    std::uint8_t chorus_send;          // 12
    std::uint8_t reverb_send;          // 13
    std::uint8_t variation_send;       // 14
    std::uint8_t vibrato_rate;         // 15
    std::uint8_t vibrato_depth;        // 16
    std::uint8_t vibrato_delay;        // 17
    std::uint8_t filter_cutoff_freq;   // 18
    std::uint8_t filter_resonance;     // 19
    std::uint8_t eg_attack_time;       // 1A
    std::uint8_t eg_decay_time;        // 1B
    std::uint8_t eg_release_time;      // 1C
    ControlDepths mw;                  // 1D..22
    ControlDepths bend;                // 23..28
    std::array<std::uint8_t, 7> reserved29; // 29..2F
    std::uint8_t rcv_pitch_bend;       // 30
    std::uint8_t rcv_channel_after_touch; // 31
    std::uint8_t rcv_program_change;   // 32
    std::uint8_t rcv_control_change;   // 33
    std::uint8_t rcv_poly_after_touch; // 34
    std::uint8_t rcv_note_message;     // 35
    std::uint8_t rcv_rpn;              // 36
    std::uint8_t rcv_nrpn;             // 37
    std::uint8_t rcv_modulation;       // 38
    std::uint8_t rcv_volume;           // 39
    std::uint8_t rcv_pan;              // 3A
    std::uint8_t rcv_expression;       // 3B
    std::uint8_t rcv_hold1;            // 3C
    std::uint8_t rcv_portamento;       // 3D
    std::uint8_t rcv_sostenuto;        // 3E
    std::uint8_t rcv_soft_pedal;       // 3F
    std::uint8_t rcv_bank_select;      // 40
    std::array<std::uint8_t, 12> scale_tuning; // 41..4C
    ControlDepths cat;                 // 4D..52
    ControlDepths pat;                 // 53..58
    std::array<AssignableControl, 2> ac; // 59..66
    std::uint8_t portamento_switch;    // 67
    std::uint8_t portamento_time;      // 68
    std::uint8_t pitch_eg_init_level;  // 69
    std::uint8_t pitch_eg_attack_time; // 6A
    std::uint8_t pitch_eg_release_level; // 6B
    std::uint8_t pitch_eg_release_time; // 6C
    std::uint8_t velocity_limit_low;   // 6D
    std::uint8_t velocity_limit_high;  // 6E
    std::uint8_t reserved6f;           // 6F
    std::uint8_t reserved70;           // 70
    std::uint8_t filter_eg_depth;      // 71 (XG2.0)
    std::uint8_t eq_bass_gain;         // 72
    std::uint8_t eq_treble_gain;       // 73
    std::array<std::uint8_t, 2> reserved74; // 74..75
    std::uint8_t eq_bass_freq;         // 76
    std::uint8_t eq_treble_freq;       // 77
    std::array<std::uint8_t, 8> reserved78; // 78..7F

    /// The XG defaults of part `part` (0..63): part 10 of each port is the drum part.
    explicit MultiPart(std::size_t part = 0) { reset(part); }
    void reset(std::size_t part);

    /// The detune as one byte, 0x80 = 0 (input).
    std::uint8_t get_detune() const {
        return static_cast<std::uint8_t>((detune_msb & 15) << 4 | (detune_lsb & 15));
    }

    std::uint8_t operator[](std::uint8_t addr) const { return bytes()[addr & 0x7F]; }
    /// Store one byte (clamped to the parameter's range); false for an unused address.
    bool write(std::uint8_t addr, std::uint8_t value);

private:
    const std::uint8_t* bytes() const { return reinterpret_cast<const std::uint8_t*>(this); }
    std::uint8_t* bytes() { return reinterpret_cast<std::uint8_t*>(this); }
};
static_assert(sizeof(MultiPart) == 0x80, "MultiPart mirrors the 08 nn 00..7F address space");

// ── Drum setup 3n rr xx ─────────────────────────────────────────────────────────────────

/// One note of a drum setup, in the order of the S-YXG2006LE's 24-byte kit entries (bytes
/// 0..15 = XG 3n rr 00..0F; then 20, 21, 24, 25, —, 50, 60, 61).
struct DrumSetup {
    std::uint8_t pitch_coarse = 64;          // 00
    std::uint8_t pitch_fine = 64;            // 01
    std::uint8_t level = 127;                // 02
    std::uint8_t alternate_group = 0;        // 03
    std::uint8_t pan = 64;                   // 04: 0 = random
    std::uint8_t reverb_send = 127;          // 05
    std::uint8_t chorus_send = 127;          // 06
    std::uint8_t variation_send = 127;       // 07
    std::uint8_t key_assign = 0;             // 08: 0 single, 1 multi
    std::uint8_t rcv_note_off = 0;           // 09
    std::uint8_t rcv_note_on = 1;            // 0A
    std::uint8_t filter_cutoff_freq = 64;    // 0B
    std::uint8_t filter_resonance = 64;      // 0C
    std::uint8_t eg_attack_rate = 64;        // 0D
    std::uint8_t eg_decay1_rate = 64;        // 0E
    std::uint8_t eg_decay2_rate = 64;        // 0F
    std::uint8_t eq_bass_gain = 64;          // 20
    std::uint8_t eq_treble_gain = 64;        // 21
    std::uint8_t eq_bass_freq = 0x0C;        // 24
    std::uint8_t eq_treble_freq = 0x36;      // 25
    std::uint8_t reserved = 0;               // (2006LE entry byte 20, no XG address)
    std::uint8_t hpf_cutoff_freq = 64;       // 50
    std::uint8_t velocity_pitch_sense = 64;  // 60
    std::uint8_t velocity_lpf_cutoff_sense = 64; // 61

    static constexpr std::size_t kBytes = 24;

    /// The first `bytes.size()` (≤ 24) fields from raw bytes, the rest at their defaults.
    static DrumSetup from_bytes(std::span<const std::uint8_t> bytes);
    /// A kit entry the bank marks as absent (all zero).
    bool is_empty() const;

    std::uint8_t operator[](std::size_t i) const { return reinterpret_cast<const std::uint8_t*>(this)[i]; }
    /// Store XG drum parameter `addr` (3n rr addr), clamped; false for an unknown address.
    bool write(std::uint8_t addr, std::uint8_t value);
    /// The field index (0..23) of XG drum parameter `addr`, or −1.
    static int index_of(std::uint8_t addr);

    bool operator==(const DrumSetup&) const = default;
};
static_assert(sizeof(DrumSetup) == DrumSetup::kBytes);

using DrumKitSetup = std::array<DrumSetup, kDrumSetupNotes>;

// ── Effect 02 01 xx ─────────────────────────────────────────────────────────────────────

/// Reverb 00..15, chorus 20..35, variation 40..60 and 70..75, stored as written.
struct EffectData {
    std::array<std::uint8_t, 0x80> bytes{};
    /// Incremented by every write of the type MSB / LSB (even with the same value) and by
    /// every reset: the effect units reload on a change (fx_system.cpp).
    std::uint32_t reverb_type_writes = 0;
    std::uint32_t chorus_type_writes = 0;
    std::uint32_t variation_type_writes = 0;
    /// Incremented by every write of the variation connection (02 01 5A, also the same value)
    /// and by every reset: the S-YXG50 re-applies the connection on each write (measured).
    std::uint32_t variation_connection_writes = 0;
    /// Resets (`restart`): the engine sets every unit up again and its variation return /
    /// sends restart from 0.
    std::uint32_t restarts = 0;
    /// SXG-Create-NF patched DLL (`--enable-mu`): a type write replaces a type the DLL has no
    /// kernel for by the nearest one it has, in the RAM, before its defaults load
    /// (fx::mu_effect_type).
    bool mu_types = false;
    /// The engine tables the type defaults come from (hvyxg/engine_tables.hpp). Without them a
    /// type write loads no defaults (the parameters stay 0).
    const EngineTables* tables = nullptr;

    EffectData() { reset(); }
    explicit EffectData(const EngineTables* t) : tables(t) { reset(); }
    /// The XG reset image (the DLLs'), counting as a type write of all three blocks.
    void reset();
    /// Count a reload of every unit (type re-sent, connection re-applied) without changing
    /// the RAM: what a reset does after loading the image.
    void restart();
    /// Store one byte. A reverb / chorus / variation type write also loads that type's
    /// default parameters (as both DLLs do) and counts.
    bool write(std::uint8_t addr, std::uint8_t value);

    std::uint8_t operator[](std::uint8_t addr) const { return bytes[addr & 0x7F]; }
    std::uint8_t variation_connection() const { return bytes[0x5A]; }
    std::uint8_t variation_part() const { return bytes[0x5B]; }
    std::uint16_t variation_param(std::size_t i) const {
        return i < 10 ? static_cast<std::uint16_t>(bytes[0x42 + 2 * i] * 128 + (bytes[0x43 + 2 * i] & 0x7F))
                      : bytes[0x70 + (i - 10)];
    }

private:
    void load_reverb_defaults();
    void load_chorus_defaults();
    void load_variation_defaults();
};

// ── Insertion 03 nn xx (XG2.0) ──────────────────────────────────────────────────────────

struct EffectInsertion {
    std::array<std::uint8_t, 0x80> bytes{};
    std::uint32_t type_writes = 0;
    /// As EffectData::tables.
    const EngineTables* tables = nullptr;

    EffectInsertion() { reset(); }
    explicit EffectInsertion(const EngineTables* t) : tables(t) { reset(); }
    /// Distortion (49 00) with its defaults, part off.
    void reset();
    bool write(std::uint8_t addr, std::uint8_t value);

    std::uint8_t operator[](std::uint8_t addr) const { return bytes[addr & 0x7F]; }
    std::uint8_t type_msb() const { return bytes[0x00]; }
    std::uint8_t type_lsb() const { return bytes[0x01]; }
    std::uint8_t part() const { return bytes[0x0C]; }
    /// Parameter `i` (0..15) in the layout of the current type.
    std::uint16_t param(std::size_t i) const;

    /// Types whose parameters 1..10 are 14-bit values at 30..43 instead of single bytes at
    /// 02..0B (the delays, the +DELAY combinations, V-distortion; MU2000 firmware).
    static bool requires_msb(std::uint8_t msb);

private:
    void load_defaults();
};

using EffectInsertions = std::array<EffectInsertion, kEffectInsertionSize>;

// ── Multi EQ 02 40 xx (XG2.0) ───────────────────────────────────────────────────────────

/// The five-band master EQ. Neither DLL renders it (docs/ENGINE.md); ranges, defaults and the
/// type presets are the MU2000's (XG spec ranges; presets read back from the MU2000 firmware).
struct MultiEq {
    /// 00 type (0 flat, 1 jazz, 2 pops, 3 rock, 4 classic); then per band k (0..4) at
    /// 01 + 4k: gain (0x34..0x4C = −12..+12 dB), frequency (XG EQ table index), Q (1..120 =
    /// 0.1..12.0); 04 / 14: shape of band 1 / 5 (0 shelf, 1 peak). 08, 0C, 10 are reserved.
    std::array<std::uint8_t, 0x15> bytes{};

    struct Band {
        std::uint8_t gain, freq, q;
        bool peak;  // bands 2..4 always
    };
    static constexpr std::size_t kBands = 5;

    MultiEq() { reset(); }
    /// Type 0 (flat) with its band values.
    void reset();
    /// Store one byte (clamped); writing the type loads its preset (as the MU2000 firmware).
    bool write(std::uint8_t addr, std::uint8_t value);
    std::uint8_t operator[](std::uint8_t addr) const { return addr < bytes.size() ? bytes[addr] : 0; }
    Band band(std::size_t k) const;
    /// All five gains at 0 dB.
    bool flat() const;
    bool operator==(const MultiEq&) const = default;

    /// The band values (gain, frequency, Q × 5) the type write loads (MU2000 firmware).
    static const std::array<std::array<std::uint8_t, 15>, 5> kTypePresets;
};

// ── Multi part extension 0A nn xx (XG2.0) ───────────────────────────────────────────────

/// The multi part block at 0A nn. The MU2000 accepts only the HPF cutoff (0A nn 20) there
/// [meas: S-MU2000 work RAM after a write of 0, 1, 0x20, 0x40, 0x7F to each of 0A 00 00..7F];
/// the S-YXG2006LE also knows 00, 01, 10 and 40..45 (its 0A address map).
struct MultiPartExt {
    /// 0A nn 01: mono note priority, 0 = last note, 1 = highest note (S-YXG2006LE part
    /// parameter 0x6f; its other engines ignore it).
    std::uint8_t mono_priority = 0;
    /// 0A nn 20: HPF cutoff, 0x40 = the voice's own (none in either bank); above it raises a
    /// 12 dB/oct high-pass (eq.hpp).
    std::uint8_t hpf_cutoff = 64;
    /// 0A nn 40..45: S-YXG2006LE level control of MW, pitch bend, CAT, PAT, AC1, AC2 (its part
    /// parameters 0x72..0x77, default 64 = none): a second amplitude term per controller
    /// that scales the level by the controller (127: the level follows it, 0 at the controller's
    /// minimum is silence; 0: the inverse) [meas]. The
    /// S-YXG50 and the MU2000 have no such parameters. Neither DLL's 0A nn 00 / 10 has an
    /// audible effect [meas], so they are not stored.
    std::array<std::uint8_t, 6> level_control{64, 64, 64, 64, 64, 64};

    void reset() { *this = MultiPartExt{}; }
    bool write(std::uint8_t addr, std::uint8_t value);
};

} // namespace hvyxg::midi::xg
