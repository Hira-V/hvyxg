// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Translated from SXG-Create-NF, Copyright (c) 2026 Soundshock, BSD-3-Clause (THIRD_PARTY.md)
#pragma once
// Ports of SXG-Create-NF utility/scaling.py, drumtrim.py and voicetrim.py (BSD-3-Clause,
// Copyright 2026 Soundshock): the MU128-engine element scaling tables, and the measured drum
// and voice level trims (keyed by CRC-32 of the source records).

#include <array>
#include <cstdint>
#include <span>
#include <tuple>
#include <vector>

#include "syxg50/mu_rom/model.hpp"

namespace hvyxg::syxg50::mu_rom {

/// scaling.Table_From_Bytes: 8 element bytes (4 break points, 4 offsets) → 128 notes.
std::array<int, 128> scaling_table(const std::uint8_t* b);
/// scaling.Inverse: 128 notes → (break points, offsets, notes that differ).
std::tuple<std::array<int, 4>, std::array<int, 4>, int> scaling_inverse(const std::array<int, 128>& t);

/// drumtrim.Trimmed_Level → (level byte, boost dB).
std::pair<int, double> drum_trimmed_level(int level, std::span<const std::uint8_t> raw42);

/// voicetrim.Voice_Key / Trim_Levels / Key_Trim.
std::uint32_t voice_key(std::span<const std::uint8_t> raw);
std::pair<std::vector<int>, double> voice_trim_levels(std::uint32_t key, const std::vector<int>& levels);
void voice_key_trim(std::uint32_t key, Bytes& edata);

} // namespace hvyxg::syxg50::mu_rom
