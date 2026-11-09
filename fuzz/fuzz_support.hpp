// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// Shared helpers of the fuzz targets (fuzz/README.md): an input reader, small synthetic banks
// built from scratch (no bytes of the Yamaha banks), and a driver that plays MIDI from fuzz
// bytes.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "hvyxg/bank.hpp"
#include "hvyxg/engine_tables.hpp"
#include "hvyxg/synth.hpp"

namespace fuzz {

/// Reads the fuzz input front to back; past the end every read returns 0.
class Input {
public:
    Input(const std::uint8_t* data, std::size_t size) : p_(data), n_(size) {}
    bool empty() const { return n_ == 0; }
    std::size_t size() const { return n_; }
    std::uint8_t u8() {
        if (n_ == 0) return 0;
        --n_;
        return *p_++;
    }
    std::uint16_t u16() { return static_cast<std::uint16_t>(u8() | (u8() << 8)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(u16()) | static_cast<std::uint32_t>(u16()) << 16; }
    std::span<const std::uint8_t> take(std::size_t k) {
        k = std::min(k, n_);
        const std::span<const std::uint8_t> s(p_, k);
        p_ += k;
        n_ -= k;
        return s;
    }
    std::span<const std::uint8_t> rest() { return take(n_); }

private:
    const std::uint8_t* p_;
    std::size_t n_;
};

/// Synthetic S-YXG50 bin table (classic layout, or SXG-Create-NF's big layout with its MAP2
/// appendix) and its plain wave table.
std::vector<std::uint8_t> syxg50_bin(bool big);
std::vector<std::uint8_t> syxg50_wave();
/// Synthetic S-YXG2006LE voice table and its encrypted wave table.
std::vector<std::uint8_t> syxg2006le_bin();
std::vector<std::uint8_t> syxg2006le_wave();

/// Bank `which` for the MIDI targets: 0 S-YXG50, 1 S-YXG2006LE, 2 S-YXG50 MU (big layout).
/// The real banks when HVYXG_TBL_BIN / _DATA, HVYXG_2006LE_BIN / _DATA, HVYXG_MU_BIN / _DATA
/// are set (better coverage of the tone modules), else the synthetic ones. Loaded once.
/// The engine tables from the DLLs under $HVYXG_DLLS, loaded once; exit(2) when they are missing.
std::shared_ptr<const hvyxg::EngineTables> need_tables();

std::shared_ptr<const hvyxg::Bank> bank(int which);

/// Synth options from one config byte (model, DLL-exact, ports, polyphony, interpolation,
/// steal fade).
hvyxg::Synth::Options options(std::uint8_t config, std::shared_ptr<const hvyxg::Bank> b);

/// Render `frames` frames and abort on a non-finite sample.
void render(hvyxg::Synth& s, std::size_t frames);

/// Play a fixed probe set on a freshly loaded bank: program / bank / drum lookups, notes over
/// the key and velocity range, controllers, then render. `seed` varies the probes.
void probe_bank(const std::shared_ptr<const hvyxg::Bank>& b, std::uint8_t seed);

/// Interpret the remaining input as a program of MIDI messages, SysEx (with XG / GS / universal
/// headers), stream bytes, renders and resets.
void play(hvyxg::Synth& s, Input& in);

} // namespace fuzz
