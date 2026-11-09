// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// Constant tables of the S-YXG2006LE voice engine, read at runtime from your S-YXG2006LE DLL
// (dll_tables.cpp, dll/tables.hpp).

#include <array>
#include <cstdint>

namespace hvyxg::syxg2006le::tbl::dll {

struct Tables {
    /// Amp EG attack time table S_att (float, integral values).
    std::array<std::int32_t, 128> kAttackTime;

    /// EG decay time table S_dec (float, integral values).
    std::array<std::int32_t, 128> kDecayTime;

    /// Amp EG release time table S_rel (float, integral values).
    std::array<std::int32_t, 128> kReleaseTime;

    /// XG EG attack > 64: attack-rate cap by v - 64 (u16 entries).
    std::array<std::uint8_t, 64> kAttackCap;

    /// XG EG release > 64: release-rate cap by v - 64 (u16 entries).
    std::array<std::uint8_t, 64> kReleaseCap;

    /// XG PEG attack / release time > 64: rate cap by v - 64.
    std::array<std::uint8_t, 64> kPegTimeCap;

    /// XG EG attack / decay / release v: filter-EG rate floor (v < 64) or cap (v > 64) (u16 entries).
    std::array<std::uint8_t, 128> kFegRateLimit;

    /// XG vibrato rate → LFO speed value limit (int32 entries).
    std::array<std::int16_t, 128> kVibratoRate;

    /// XG vibrato delay → LFO delay limit, 10 ms (u16 entries).
    std::array<std::int16_t, 128> kVibratoDelay;

    /// Amplitude 0..255 → attenuation units (0.1875 dB).
    std::array<std::uint16_t, 256> kVolumeAtt;

    /// Send level 2·v → attenuation units (0.375 dB), 0 → 255.
    std::array<std::uint8_t, 256> kSendAtt;

    /// Part detune (cents).
    std::array<std::int8_t, 256> kDetune;

    /// Voice type 6: XG EG attack > 64 attack-rate cap by v - 64.
    std::array<std::uint8_t, 64> kAttackCap6;

    /// Voice type 6: XG EG release > 64 rate cap, indexed by kReleaseOffset6[program] + v - 64.
    std::array<std::uint8_t, 128> kReleaseCap6;

    /// Voice type 6: per-program offset into kReleaseCap6.
    std::array<std::uint8_t, 128> kReleaseOffset6;

    /// Portamento: seconds per octave by rate 127 - XG portamento time (float).
    std::array<float, 128> kPortamentoTime;

    /// LFO initial phase index → degrees (int32 entries).
    std::array<std::int16_t, 36> kLfoPhaseDegrees;

    /// Controller amplitude terms A[depth][value] in attenuation units (i16[128][128]); depth 64 = 0, depths below 64 attenuate as the value rises.
    std::array<std::array<std::int16_t, 128>, 128> kAmpControl;

    /// Filter cutoff ceiling by resonance, (entry << 8) in 1/65536 octave:
    /// one row per type class (types 0/8 → 0, 1/5 → 1, 2/3/4/9 → 2, 6/7 → 3),
    /// rows, indexed by the resonance.
    std::array<std::array<std::uint16_t, 256>, 4> kFilterCeiling;


    /// Voice type 6: part detune scale per key for detune ≥ 0 (cents = T[key]·d / 4096, toward zero).
    std::array<std::uint16_t, 128> kDetuneKeyUp;

    /// Voice type 6: the same for detune < 0.
    std::array<std::uint16_t, 128> kDetuneKeyDown;

    /// Pan 0..128 → attenuation (0.375 dB).
    std::array<std::uint8_t, 129> kPan;
    /// LFO speed index → speed value (94 in the DLL; the last two stay 0).
    std::array<std::int16_t, 96> kLfoSpeed;
    /// Key-tracking scale per mode, percent.
    std::array<std::int32_t, 8> kKeyTrackScale;
    /// The GS layer's rows of XG values (midi/gs_effects.cpp), 19 bytes each. Reverb macro /
    /// character: type MSB, LSB, GS time, parameters 2..4, GS pre-LPF,
    /// parameters 6..10 (not sent), parameters 11..15 (sent), parameter 16 (not sent), GS level.
    /// Chorus macro: type MSB, LSB, parameters 1..16, return.
    std::array<std::array<std::uint8_t, 19>, 8> kGsReverb;
    std::array<std::array<std::uint8_t, 19>, 8> kGsChorus;
};

} // namespace hvyxg::syxg2006le::tbl::dll
