// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// Tables of the reverb / chorus control and of the engine-side effect mixer, shared by
// syxg50.dll and S-YXG2006LE.DLL (generated into fx_tables.cpp).

#include <array>
#include <cstddef>
#include <cstdint>

namespace hvyxg::fx {

struct Tables {
    // DSP word format per coefficient register, category 0 (0 = s10/512, 1 = s10/128, 2 = float, 0xFF = unused).
    std::array<std::uint8_t, 0xC0> kCoefFormat;
    // reverb algorithm → program row.
    std::array<std::uint8_t, 19> kReverbProgramIndex;
    // reverb programs, 0x48 words per row: 45 coefficient words (order kReverbCoefList), 27 addresses (order kReverbAddrList).
    std::array<std::uint16_t, 4 * 0x48> kReverbProgram;
    // coefficient registers of the reverb.
    std::array<std::uint8_t, 0x2D> kReverbCoefList;
    // address registers of the reverb.
    std::array<std::uint8_t, 0x1B> kReverbAddrList;
    // chorus algorithm → program row.
    std::array<std::uint8_t, 21> kChorusProgramIndex;
    // chorus programs, 0x26 words per row: 28 coefficient words (order kChorusCoefList), 10 addresses.
    std::array<std::uint16_t, 7 * 0x26> kChorusProgram;
    // coefficient registers of the chorus.
    std::array<std::uint8_t, 0x1C> kChorusCoefList;
    // address registers of the chorus.
    std::array<std::uint8_t, 10> kChorusAddrList;
    // mixer register reset words (order kMixerCoefList).
    std::array<std::int16_t, 0x17> kMixerProgram;
    // mixer registers.
    std::array<std::uint8_t, 0x17> kMixerCoefList;
    // reverb time p1 → base row of kCombGain; element 0 is the byte before the table (read for time − 1).
    std::array<std::uint8_t, 72> kReverbTimeBase;
    // per time set (4 combs each) offset into kCombGain.
    std::array<std::uint8_t, 16> kReverbTimeOffset;
    // comb gain pairs {fir/512, input/512}.
    std::array<std::uint16_t, 2 * 208> kCombGain;
    // reverb algorithm → time / geometry set.
    std::array<std::uint16_t, 19> kReverbTimeSet;
    // per set 4 × {tap offset, comb length} in 10 µs units.
    std::array<std::uint16_t, 8 * 8> kDiffusionGeometry;
    // per comb stereo spread of the taps.
    std::array<std::uint16_t, 4> kDiffusionSpread;
    // diffusion p2 → spread scale (/128).
    std::array<std::uint8_t, 11> kDiffusionDepth;
    // White Room/Tunnel/Basement dimension → comb length (10 µs).
    std::array<std::uint16_t, 200> kRoomSize;
    // dimension → comb gain row offset (+0x38).
    std::array<std::uint8_t, 200> kRoomTimeOffset;
    // per set 4 × {all-pass length (10 µs), gain word}: input AP1, AP2, output L, output R.
    std::array<std::uint16_t, 8 * 8> kAllpassGeometry;
    // density → enabled all-pass mask.
    std::array<std::uint8_t, 4> kDensityMask;
    // chorus algorithm → output gain (/64).
    std::array<std::uint8_t, 21> kChorusOutGain;
    // register lists and small tables of the reverb control (see fx_tables.hpp for offsets).
    std::array<std::uint8_t, 0xC0> kControlLists;
    // all-pass coefficient registers, 4 per stage {in, in·g, fb, out}.
    std::array<std::uint8_t, 16> kAllpassCoefList;
    // register lists of the chorus control.
    std::array<std::uint8_t, 0x30> kChorusLists;
    // chorus LFO frequency → coefficient word (fmt 2).
    std::array<std::uint16_t, 128> kLfoWord;
    // chorus delay offset → 0.1 ms.
    std::array<std::uint16_t, 128> kChorusDelay;
    // reverb initial / rev delay → 0.1 ms (XG table #5).
    std::array<std::uint16_t, 64> kInitialDelay;
    // ER/rev balance and dry/wet curve, 0..512.
    std::array<std::int16_t, 129> kLevelCurve;
    // constant-power pan curve (right side), 0..512.
    std::array<std::int16_t, 129> kPanCurve;
    // reverb algorithm → early-reflection gain scale (/256).
    std::array<std::uint8_t, 19> kErScale;
    // tan(π f / 44100) for the XG EQ frequencies.
    std::array<float, 61> kTan44100;
    // filter gain 52..76 → linear (1 dB steps).
    std::array<float, 25> kShelfGain;
    // reverb parameter {min, max}; {0, 0} = ignored.
    std::array<std::uint16_t, 32> kReverbLimits;
    // White Room/Tunnel/Basement parameter limits.
    std::array<std::uint16_t, 32> kRoomReverbLimits;
    // chorus / celeste parameter limits.
    std::array<std::uint16_t, 32> kChorusLimits;
    // mixer parameter limits.
    std::array<std::uint16_t, 32> kMixerLimits;
    // flanger parameter limits (delay offset ≤ 63).
    std::array<std::uint16_t, 32> kFlangerLimits;
    // Variation.
    // DSP word format per coefficient register and variation category (0xC0 per category).
    std::array<std::uint8_t, 4 * 0xC0> kVarCoefFormat;
    // coefficient registers of the variation.
    std::array<std::uint8_t, 0x4F> kVarCoefList;
    // variation category 0 type → program row.
    std::array<std::uint8_t, 24> kVarProgramIndex0;
    // variation category 1 type → program row.
    std::array<std::uint8_t, 4> kVarProgramIndex1;
    // variation category 2 type → program row.
    std::array<std::uint8_t, 12> kVarProgramIndex2;
    // variation category 3 type → program row.
    std::array<std::uint8_t, 16> kVarProgramIndex3;
    // variation category 0 programs, 106 words per row: 79 coefficient words (order kVarCoefList), 27 addresses (registers 36..62).
    std::array<std::uint16_t, 11 * 106> kVarProgram0;
    // variation category 1 programs.
    std::array<std::uint16_t, 3 * 106> kVarProgram1;
    // variation category 2 programs.
    std::array<std::uint16_t, 4 * 106> kVarProgram2;
    // variation category 3 programs.
    std::array<std::uint16_t, 5 * 106> kVarProgram3;
    // variation category 0 default parameters 1..16 per type (the engines' copies are identical).
    std::array<std::uint16_t, 23 * 16> kVarDefaults0;
    // variation category 1 defaults.
    std::array<std::uint16_t, 3 * 16> kVarDefaults1;
    // variation category 2 defaults.
    std::array<std::uint16_t, 9 * 16> kVarDefaults2;
    // variation category 3 defaults.
    std::array<std::uint16_t, 13 * 16> kVarDefaults3;
    // variation output gain (/64) per category (23 per category) and type.
    std::array<std::uint8_t, 4 * 23> kVarOutGain;
    // variation MSB 0x00..0x0B × LSB 0..9 → type within the category.
    std::array<std::uint8_t, 120> kVarTypeSub;
    // variation MSB 0x00..0x0B × LSB 0..9 → category.
    std::array<std::uint8_t, 120> kVarTypeCat;
    // variation MSB 0x14 (Karaoke) × LSB → type.
    std::array<std::uint8_t, 10> kVarTypeSub14;
    // variation MSB 0x14 × LSB → category.
    std::array<std::uint8_t, 10> kVarTypeCat14;
    // variation MSB 0x40..0x4E × LSB 0..9 → type.
    std::array<std::uint8_t, 150> kVarTypeSub40;
    // variation MSB 0x40..0x4E × LSB 0..9 → category.
    std::array<std::uint8_t, 150> kVarTypeCat40;
    /// 24 tables of 16 × {min, max}, in the DLLs' order (·n).
    // variation parameter limits, S-YXG2006LE.
    std::array<std::uint16_t, 24 * 32> kVarLimits2006LE;
    // variation parameter limits, S-YXG50.
    std::array<std::uint16_t, 24 * 32> kVarLimitsSyxg50;
    // register lists and small tables of the variation control (see var_list()).
    std::array<std::uint8_t, 0x398> kVarListsA;
    // register lists and small tables of the variation control.
    std::array<std::uint8_t, 0x230> kVarListsB;
    // level 0..127 → ≈ (v/127)², the delay tap levels.
    std::array<float, 128> kLevelSquare;
    // Variation category 2.
    // early reflections: pattern p1 + 6 · type → all-pass geometry set (kAllpassGeometry row).
    std::array<std::uint16_t, 30> kErSet;
    // Variation category 3.
    // karaoke delay time → 0.1 ms (0.1 … 400 ms).
    std::array<std::uint16_t, 128> kKaraokeDelay;

    /// A register list or small table of the variation control, by its offset in the lists' block:
    /// kVarListsA from 0, kVarListsB from 0x3a0 (after an 8-byte gap).
    const std::uint8_t* var_list(std::uint32_t offset) const {
        if (offset >= 0x3a0u) return kVarListsB.data() + (offset - 0x3a0u);
        return kVarListsA.data() + offset;
    }

    /// XG reverb / chorus type → internal algorithm (LSB > 9 reads LSB 0; see `reverb_algorithm`).
    // reverb MSB 0x00..0x04 × LSB 0..9 → internal algorithm (0 = off).
    std::array<std::uint8_t, 50> kReverbTypeMap;
    // reverb MSB 0x10..0x13 × LSB 0..9 → internal algorithm.
    std::array<std::uint8_t, 40> kReverbTypeMapRoom;
    // chorus MSB 0x41..0x43 × LSB 0..9 → internal algorithm.
    std::array<std::uint8_t, 30> kChorusTypeMap;
    /// XG default parameters 1..16 per reverb (19) / chorus (21) algorithm.
    // reverb algorithm → XG default parameters 1..16 (loaded into the RAM by a type write).
    std::array<std::uint16_t, 19 * 16> kReverbDefaults;
    // chorus algorithm → XG default parameters 1..16.
    std::array<std::uint16_t, 21 * 16> kChorusDefaults;

    // attenuation (1/16 octave) → mixer word, 512·2^(−a/16).
    std::array<std::uint16_t, 128> kMixerWord;
    // XG level 0..127 → attenuation.
    std::array<std::uint8_t, 128> kMixerLevel;
    // XG pan → right-side attenuation; the left side reads kMixerPan[128 − pan].
    std::array<std::uint8_t, 128> kMixerPan;
    /// S-YXG50 only: volume 0..127 → attenuation, the insertion-variation return.
    // S-YXG50 only: master / part volume / expression 0..127 → attenuation (1/16 octave).
    std::array<std::uint8_t, 128> kMixerVolume;
    /// S-YXG50 only: variation MSB → input class.
    // S-YXG50 only: variation MSB → input class (stereo-input test).
    std::array<std::uint8_t, 128> kVarInputClass;

    /// The 14 early-reflection tap address registers, in the order of the tap gain list (a
    /// string literal the variation control passes as a register list).
    std::array<std::uint8_t, 14> kErTapAddr;
    // The GS layer of both DLLs (midi/gs_effects.cpp).
    /// GS reverb time → XG reverb time: Room 1 and Plate, the other characters.
    std::array<std::uint8_t, 128> kGsTimeRoom1Plate;
    std::array<std::uint8_t, 128> kGsTimeOther;
    /// GS reverb pre-LPF 0..7 → XG reverb LPF (parameter 5).
    std::array<std::uint8_t, 8> kGsPreLpf;
};

/// Offsets into `kControlLists` (+ offset).
namespace lists {
inline constexpr std::size_t kHpf = 0x00;            // {b0, b1, a}
inline constexpr std::size_t kLpf = 0x04;            // {b0, b1, a}
inline constexpr std::size_t kDiffusion = 0x08;      // 4 × {comb write, comb read, tap, tap}
inline constexpr std::size_t kCombCoef = 0x18;       // 4 × {in, fir, fir'}
inline constexpr std::size_t kAllpassAddr = 0x24;    // 4 × {write, read}
inline constexpr std::size_t kOutAllpassRead = 0x2B; // {A23}
inline constexpr std::size_t kDelayAddr = 0x2D;      // {A8, A63}
inline constexpr std::size_t kErRev = 0x30;          // 10 registers
inline constexpr std::size_t kFeedback = 0x3A;       // {51}
inline constexpr std::size_t kHpfTrim = 0x3C;        // s8 per algorithm, dB
inline constexpr std::size_t kRoomRowSet = 0x50;     // algorithm & 7 → dimension row
inline constexpr std::size_t kRoomOrderSet = 0x58;   // algorithm & 7 → comb order row
inline constexpr std::size_t kRoomOrder = 0x61;      // {comb 2, comb 3, comb 4 spread index}
inline constexpr std::size_t kRoomRow = 0x76;        // 12-byte rows, 3 × s16 time skew
inline constexpr std::size_t kMute = 0xA0;           // {11, 87, 188} fade target, {12, 88, 189} pole
} // namespace lists

/// Offsets into `kChorusLists` (+ offset).
namespace chorus_lists {
inline constexpr std::size_t kEqLowL = 0x00;
inline constexpr std::size_t kEqHighL = 0x04;
inline constexpr std::size_t kEqLowR = 0x08;
inline constexpr std::size_t kEqHighR = 0x0C;
inline constexpr std::size_t kLfo = 0x0F;      // {9}
inline constexpr std::size_t kPhase = 0x10;    // {37}
inline constexpr std::size_t kDepth = 0x14;    // {26, 40, 55}
inline constexpr std::size_t kFeedback = 0x18; // {79, 66}
inline constexpr std::size_t kTapAddr = 0x1D;  // {9, 10, 19, 20}
inline constexpr std::size_t kLineAddr = 0x21; // {22, 14, 15, 25, 26}
inline constexpr std::size_t kInputMode = 0x26; // {80}
} // namespace chorus_lists

/// XG EQ frequency table #3 (Hz), the frequencies of `kTan44100`.
inline constexpr std::array<float, 61> kEqFrequency = {
    20,   22,   25,   28,   32,   36,   40,   45,   50,    56,    63,    70,    80,
    90,   100,  110,  125,  140,  160,  180,  200,  225,   250,   280,   315,   355,
    400,  450,  500,  560,  630,  700,  800,  900,  1000,  1100,  1200,  1400,  1600,
    1800, 2000, 2200, 2500, 2800, 3200, 3600, 4000, 4500,  5000,  5600,  6300,  7000,
    8000, 9000, 10000, 11000, 12000, 14000, 16000, 18000, 20000};

} // namespace hvyxg::fx
