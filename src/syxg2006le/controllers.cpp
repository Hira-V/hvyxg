// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "syxg2006le/controllers.hpp"

#include <algorithm>
#include <cstdlib>

#include "midi/part.hpp"
#include "syxg2006le/tbl/dll_tables.hpp"

namespace hvyxg::syxg2006le {

namespace {


/// identity with 127 → 128.
int t128(int v) { return v >= 127 ? 128 : std::max(v, 0); }
/// 2·i with 127 → 256.
int v256(int v) { return v >= 127 ? 256 : 2 * std::max(v, 0); }

int amp_term(const tbl::dll::Tables& tb, int depth, int value) {
    return tb.kAmpControl[static_cast<std::size_t>(depth & 0x7F)][static_cast<std::size_t>(value & 0x7F)];
}

struct Depths {
    int pitch, filter, amp, pmod, fmod, amod;
};

template <typename T> Depths depths(const T& d) {
    return {d.pitch_control, d.filter_control, d.amplitude_control, d.lfo_pmod_depth,
            d.lfo_fmod_depth, d.lfo_amod_depth};
}

/// The level control term (0A nn 40..45) of a 7-bit source `value` at `depth`:
/// 64 gives 0; 127 makes the level follow the source, 0 the inverse. Attenuation units.
int level_term(const tbl::dll::Tables& tb, int value, int depth) {
    int d = depth - 64;
    if (d == 63) d = 64;
    const int t = t128(value);
    const int x = d < 0 ? ((d * t) >> 6) + 0x80 : ((d * t) >> 6) + (64 - d) * 2;
    return tb.kVolumeAtt[static_cast<std::size_t>(std::clamp(2 * x, 0, 255))];
}

struct Sums {
    const tbl::dll::Tables& tb;
    int amp = 0, filter = 0, pitch = 0, pmod = 0, fmod = 0, amod = 0;
    /// The level control terms, added after the amplitude slot's clamp.
    int level = 0;

    /// A 7-bit source.
    void add(int value, const Depths& d) {
        const int v = t128(value);
        pitch += v * (d.pitch - 64) * 100 >> 7;
        filter += (t128(d.filter) - 64) * v;
        amp += amp_term(tb, d.amp, value);
        pmod += v * t128(d.pmod);
        fmod += v * t128(d.fmod);
        amod += v * t128(d.amod);
    }
    /// Pitch bend, centred 14-bit.
    void add_bend(int bend, const Depths& d) {
        pitch += bend * (d.pitch - 64) * 25 >> 11;
        filter += bend * (t128(d.filter) - 64) >> 6;
        const int v = (bend * 127) >> 13;
        amp += v >= 0 ? amp_term(tb, d.amp, v) : -amp_term(tb, d.amp, -v);
        pmod += std::abs(bend * t128(d.pmod)) >> 6;
        fmod += std::abs(bend * t128(d.fmod)) >> 6;
        amod += std::abs(bend * t128(d.amod)) >> 6;
    }
};

} // namespace

int master256(const EngineState& state) { return v256(effective_master_volume(state)); }

ControllerSources controller_sources(const midi::Part& part) {
    const auto& ctl = part.controller;
    return {.bend = part.pitchbend,
            .cat = part.cat_value,
            .mw = ctl.modulation,
            .ac = {ctl.ac[0], ctl.ac[1]}};
}

ControllerSums controller_sums(const tbl::dll::Tables& tb, const midi::Part& part, const ControllerSources& src,
                               const midi::xg::System& system, std::uint8_t key, bool drum,
                               bool insertion, int master256_value) {
    const auto& ram = *part.ram;
    const auto& ctl = part.controller;
    Sums s{tb};
    int bend = static_cast<int>(src.bend) - 0x2000;
    // the centred value +0x1FFF (the top of the range)
    // counts as +0x2000 [meas: PB amp / FMod at 16383 −53 / −45 dB → exact; −1 stays −1].
    if (bend == 0x1FFF) bend = 0x2000;
    s.add_bend(bend, depths(ram.bend));
    s.add(src.cat, depths(ram.cat));
    s.add(src.mw, depths(ram.mw));
    s.add(src.ac[0], depths(ram.ac[0]));
    s.add(src.ac[1], depths(ram.ac[1]));
    // Poly aftertouch: normal parts, keys 0x24..0x61 only.
    const bool pat_key = ram.part_mode == 0 && key >= 0x24 && key <= 0x61;
    if (pat_key) s.add(part.pat_values[key], depths(ram.pat));
    // Level controls 0A nn 40..45 (MW, bend, CAT, PAT, AC1, AC2; slot 6,
    // for the bend from |bend| >> 6). A depth of 64 adds nothing. The PAT term
    // needs the part's PAT flag, which a write of 0A nn 43 sets.
    if (part.ram_ext) {
        const auto& lc = part.ram_ext->level_control;
        const auto add_level = [&](int value, std::uint8_t depth) {
            if (depth != 64) s.level += level_term(tb, value, depth);
        };
        add_level(src.mw, lc[0]);
        add_level(std::min(std::abs(bend) >> 6, 127), lc[1]);
        add_level(src.cat, lc[2]);
        if (pat_key) add_level(part.pat_values[key], lc[3]);
        add_level(src.ac[0], lc[4]);
        add_level(src.ac[1], lc[5]);
    }

    ControllerSums out;
    // Slot 0: expression × volume × XG master volume.
    const int idx = insertion ? 255
                              : std::min(((v256(ctl.expression) * v256(ram.volume)) >> 8) *
                                             (master256_value < 0 ? v256(system.master_volume) : master256_value) >> 8,
                                         255);
    int amp = tb.kVolumeAtt[static_cast<std::size_t>(idx)];
    if (amp < 0xFF) amp += s.amp;
    out.amp = std::clamp(amp, 0, 0x101);
    // the level control terms on top, capped at 0x101 (silence).
    if (s.level) out.amp = std::min(out.amp + s.level, 0x101);
    // Slot 1: the part cutoff is (v − 64)·75 cents.
    out.filter = std::clamp((s.filter + (t128(ram.filter_cutoff_freq) - 64) * 128) * 4800 >> 13,
                            -4800, 4800);
    // Slot 2: controllers + RPN fine / coarse tuning + master tune.
    const int fine14 = (part.rpn.fine_msb << 7) | part.rpn.fine_lsb;
    int fine = (fine14 - 0x2000) >> 6;
    if (fine == 127) fine = 128;
    int pitch = s.pitch + (fine * 100 >> 7) + (part.rpn.coarse - 64) * 100;
    if (drum) {
        pitch += (ram.note_shift - 64) * 100;
    } else {
        // XG master tune: (v − 0x400) / 10 cents; the first nibble is ignored.
        const int tune = (system.master_tune2 << 8) | (system.master_tune3 << 4) | system.master_tune4;
        pitch += (tune - 0x400) / 10 + (system.master_coarse - 64) * 100;
    }
    out.pitch = std::clamp(pitch, -9600, 9600);
    out.pmod = s.pmod * 10 >> 7;
    out.fmod = s.fmod * 75 >> 8;
    out.amod = s.amod >> 8;
    return out;
}

} // namespace hvyxg::syxg2006le
