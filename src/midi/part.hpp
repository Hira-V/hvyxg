// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// One part as the tone modules see it: its XG multi-part RAM, live controller values,
// pitch bend / aftertouch, RPN tunings, the program latched at the last program change and
// its own drum setup. Owned by the front end at a stable address for the synth's life.

#include <array>
#include <cstddef>
#include <cstdint>

#include "midi/xg_ram.hpp"

namespace hvyxg::midi {

/// Parts: 16 per port, up to 4 ports (the DLLs have one port).
inline constexpr std::size_t kPartsPerPort = 16;
inline constexpr std::size_t kMaxPorts = 4;
inline constexpr std::size_t kMaxPartSize = kPartsPerPort * kMaxPorts;

/// A program as latched at a program change: bank select MSB / LSB and the program number.
/// Drum-mode parts whose bank MSB names no kit bank carry `kDrumBankMsbGs` (bank.hpp).
struct Program {
    bool valid = false;
    std::uint8_t bank_msb = 0;
    std::uint8_t bank_lsb = 0;
    std::uint8_t program = 0;
    bool operator==(const Program&) const = default;
};

/// Live controller values.
struct Controller {
    /// Last value of every control change number.
    std::array<std::uint8_t, 128> cc_values{};
    /// AC1 / AC2 source values: the last value received on the control number assigned at the
    /// time (`08 nn 59` / `60`). A number change keeps the value; only a later CC on the new
    /// number moves it (both DLLs, measured). 0 after every reset.
    std::array<std::uint8_t, 2> ac{};
    std::uint8_t modulation = 0;
    std::uint8_t expression = 127;
    bool sustain = false;
    bool sostenuto = false;
    bool soft = false;
    /// CC84 portamento control: the source key of the next note (−1 = none).
    int portamento_control = -1;
};

/// RPN-set values (RPN 0,0 bend range is stored in the RAM's bend pitch control).
struct Rpn {
    /// RPN 0,1 fine tune (14 bit, 0x40 0x00 = 0).
    std::uint8_t fine_msb = 0x40;
    std::uint8_t fine_lsb = 0x00;
    /// RPN 0,2 coarse tune (0x40 = 0 semitones).
    std::uint8_t coarse = 0x40;
    /// RPN 0,5 modulation depth range (GM2), semitones / cents.
    std::uint8_t mod_depth_msb = 0;
    std::uint8_t mod_depth_lsb = 0x40;
};

struct Part {
    /// 0..63 (port · 16 + part); stable.
    std::size_t id = 0;
    /// The part's XG multi-part RAM.
    const xg::MultiPart* ram = nullptr;
    /// Its XG2.0 extension block 0A nn (part HPF).
    const xg::MultiPartExt* ram_ext = nullptr;
    Controller controller;
    /// 14-bit pitch bend, 0x2000 = centre.
    std::uint16_t pitchbend = 0x2000;
    /// Channel aftertouch and poly aftertouch per key.
    std::uint8_t cat_value = 0;
    std::array<std::uint8_t, 128> pat_values{};
    Rpn rpn;
    /// Counts the messages that touch the part's sounding voices (control changes, bend,
    /// aftertouch, its XG part parameters, master volume, GM2 master tuning): the S-YXG50
    /// marks its voices for a register update on each.
    std::uint32_t touch = 0;
    /// The part volume and master volume the part gain was last computed from (the S-YXG50's
    /// reads them from the RAM at CC7 / CC11 / reset controllers / volume /
    /// master volume / resets; a parameter change whose deferred handler was skipped leaves the
    /// RAM ahead of them). Other engines: always the RAM.
    std::uint8_t gain_volume = 100;
    std::uint8_t gain_master = 127;
    /// The program latched at the last program change.
    Program program;
    /// A part playing a kit outside the drum-setup RAM (part mode 0 / 1) keeps its own copy,
    /// loaded at the program change for `drum_setup_program` and edited by its drum NRPNs.
    Program drum_setup_program;
    xg::DrumKitSetup drum_setup{};
};

} // namespace hvyxg::midi
