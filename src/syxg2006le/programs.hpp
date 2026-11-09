// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// Program selection: (bank MSB, LSB, program) → voice record, drum kits and their drum-setup
// RAM images (seg0 / seg1 / seg8 of the voice table).

#include <cstdint>
#include <optional>

#include "bank_impl.hpp"
#include "hvyxg/types.hpp"

namespace hvyxg::syxg2006le {

class BankData;

/// The voice a part plays for its latched program under `mode`, with the DLL's fallbacks
/// for banks and programs the bank does not have.
/// A drum-kit bank: MSB 127 (XG kits), 126 (SFX), 120 (GM2), 118, or hvyxg's GS kit marker.
bool is_drum_bank(std::uint8_t msb);

std::optional<std::uint16_t> resolve_voice(const BankData& bank, std::uint8_t msb,
                                           std::uint8_t lsb, std::uint8_t program,
                                           ResetMode mode = ResetMode::XG);

/// The seg8 drum kit of drum program (bank MSB, program), if that program is a drum voice.
std::optional<std::uint16_t> drum_kit(const BankData& bank, std::uint8_t msb,
                                      std::uint8_t program);

/// The XG drum-setup RAM image (notes 13..91) of seg8 kit `kit`.
DrumKitSetup kit_setup(const BankData& bank, std::uint16_t kit);

} // namespace hvyxg::syxg2006le
