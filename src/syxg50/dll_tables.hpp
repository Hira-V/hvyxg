// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// Tables of the S-YXG50 voice engine, read at runtime from your syxg50.dll or SXG-Create-NF
// plugin (dll_tables.cpp, dll/tables.hpp).

#include <array>
#include <cstdint>

namespace hvyxg::syxg50::dll {

struct Tables {
    /// TA: attack step = 2^26 / TA[r] per block
    std::array<float, 128> kAttackTime;

    /// TB: decay step = 2^27 / TB[r] per block; also LFO speed, cutoff and level glides
    std::array<float, 128> kDecayTime;

    /// TC: release step = 2^27 / TC[r] per block
    std::array<float, 128> kReleaseTime;

    /// Tlev: level 0..128 -> attenuation in 0.75 dB units
    std::array<std::uint8_t, 129> kLevelAtten;

    /// element velocity curves 0..6 (element byte 64), 128 bytes each
    std::array<std::uint8_t, 896> kVelocityCurves;

    /// PEG step per 10 ms tick (1/4 cent); -0x8000 = instant
    std::array<std::int16_t, 64> kPegStep;

    /// FEG step per 10 ms tick (1/4 cutoff unit); -0x8000 = instant
    std::array<std::int16_t, 64> kFegStep;

    /// cutoff byte -> 12-bit cutoff (256 per octave)
    std::array<std::int16_t, 128> kCutoff;

    /// XG part EG attack/decay/release -> FEG rate clamp
    std::array<std::uint8_t, 128> kFegPartRateClamp;

    /// element LFO speed (byte 7) -> s; step = 2^27 / TB[32 + s]
    std::array<std::uint8_t, 64> kLfoSpeed;

    /// part vibrato rate clamp, indexed base + part value (base 14 in XG mode, up to 38 in GS mode)
    std::array<std::uint8_t, 166> kVibratoRate;

    /// faded element vibrato depth -> PMD
    std::array<std::uint8_t, 128> kVibratoPmd;

    /// controller vibrato depth sum -> PMD (XG)
    std::array<std::uint8_t, 128> kCtrlPmdXg;

    /// controller vibrato depth sum -> PMD (GS)
    std::array<std::uint8_t, 128> kCtrlPmdGs;

    /// FMD table
    std::array<std::uint8_t, 16> kFmdTable;

    /// XG part attack / PEG attack time > 64 -> rate cap
    std::array<std::uint8_t, 64> kPartAttackRate;

    /// XG part PEG / amp release time > 64 -> rate cap (the amp release indexes offset + time - 64, offset 1 in XG mode, per program in GS mode)
    std::array<std::uint8_t, 112> kPartReleaseRate;

    /// XG part vibrato depth -> signed depth offset
    std::array<std::int8_t, 128> kVibratoDepth;

    /// part vibrato delay -> LFO ticks: three 128-entry tables, the first in XG mode, per program in GS mode
    std::array<std::uint8_t, 384> kVibratoDelay;

    /// part pan -> left pan attenuation (1.5 dB steps); right uses [127 - pan]
    std::array<std::uint8_t, 128> kPartPan;

    /// element / drum pan -> left pan attenuation; right uses [127 - pan]
    std::array<std::uint8_t, 128> kElementPan;

    /// element pan byte 65 (0..14) -> pan
    std::array<std::uint8_t, 16> kElementPanPos;

    /// element pan 15: pan by key
    std::array<std::uint8_t, 128> kKeyPan;

    /// dry / send level -> attenuation (0.375 dB units, 255 = off)
    std::array<std::uint8_t, 128> kSendAtten;

    /// chorus send attenuation added by pan
    std::array<std::uint8_t, 128> kChorusPanAtten;

    /// element byte 68 -> AEG key-on delay: the key-on waits while
    /// the 10-ms tick subtracts 20, i.e. `ceil(t / 20)` ticks
    std::array<std::uint16_t, 16> kKeyOnDelay;

    /// element byte 15 -> key tracking percent
    std::array<std::uint8_t, 8> kKeyTrack;

    /// FEG velocity sensitivity curve
    std::array<std::uint8_t, 128> kFegVelCurve0;

    /// FEG velocity sensitivity curve (element byte 5 bit 7)
    std::array<std::uint8_t, 128> kFegVelCurve1;

    /// portamento time -> cents per tick (< 24: ×2) or 8.8 step
    std::array<std::uint16_t, 128> kPortaRate;

    /// XG drum setup record of part mode 1 / keys without RAM
    std::array<std::uint8_t, 16> kDrumDefault;

    /// XG drum setup RAM template (bytes 2-10 come from the kit entry)
    std::array<std::uint8_t, 16> kDrumTemplate;

    /// drum auto-release (40 ms units) by key, kits > 9
    std::array<std::uint8_t, 128> kDrumAutoReleaseXg;

    /// drum auto-release by key, GS kits
    std::array<std::uint8_t, 128> kDrumAutoReleaseGs;

    /// drum auto-release by key, kit 7 (GS Brush)
    std::array<std::uint8_t, 128> kDrumAutoReleaseBrush;

    /// cutoff change per tick / 2 -> chip cutoff glide rate
    std::array<std::uint8_t, 128> kCutoffGlideRate;

    /// GS part vibrato depth -> depth
    std::array<std::int8_t, 128> kVibratoDepthGs;

    /// GS mode, melodic program -> vibrato rate base into kVibratoRate
    std::array<std::uint8_t, 128> kGsVibratoRateBase;

    /// GS mode, melodic program -> amp release offset into kPartReleaseRate
    std::array<std::uint8_t, 128> kGsReleaseOffset;

    /// GS mode, melodic program -> vibrato delay table offset / 2 into kVibratoDelay
    std::array<std::uint8_t, 128> kGsVibratoDelayTable;

    /// GS part cutoff offset |cutoff - 64| -> cutoff units (XG mode uses 16 per step)
    std::array<std::int16_t, 65> kGsPartCutoff;

    /// GS part detune, key-dependent scale for a positive detune
    std::array<std::uint16_t, 128> kGsDetuneUp;

    /// GS part detune, key-dependent scale for a negative detune
    std::array<std::uint16_t, 128> kGsDetuneDown;

    // The GS layer (midi/gs_effects.cpp).
    /// Reverb macro → character, pre-LPF, level, time, delay feedback, pre-delay
    std::array<std::array<std::uint8_t, 6>, 8> kGsReverbMacro;
    /// Reverb character → XG type MSB / LSB: Room 1..3, Hall 1 / 2, Plate; Delay
    /// and Panning Delay → no reverb
    std::array<std::array<std::uint8_t, 2>, 8> kGsCharacterType;
    /// Reverb character → XG reverb parameters 1..16, six characters (parameters 1 and 5 are then
    /// replaced from the GS time and pre-LPF)
    std::array<std::array<std::uint8_t, 16>, 8> kGsCharacterParams;
    /// Chorus macro → pre-LPF, level, feedback, delay, rate, depth, send to reverb
    std::array<std::array<std::uint8_t, 7>, 8> kGsChorusMacro;
    /// XG Chorus 1 parameters 1..16 every chorus macro starts from (parameters 1..4 are then replaced
    /// from the GS rate, depth, feedback and delay)
    std::array<std::uint8_t, 16> kGsChorusParams;
};

} // namespace hvyxg::syxg50::dll
