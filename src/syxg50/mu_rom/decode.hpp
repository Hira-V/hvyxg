// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Translated from SXG-Create-NF, Copyright (c) 2026 Soundshock, BSD-3-Clause (THIRD_PARTY.md)
#pragma once
// The MU128-engine ROM decoder (MU1000 / MU2000 / MU128), ported from SXG-Create-NF
// utility/decMU1000.py, decMU90.py and decBase.py (BSD-3-Clause, Copyright 2026 Soundshock):
// program-ROM tables → the converter's object model (model.hpp).

#include <cstdint>
#include <span>
#include <string>

#include "syxg50/mu_rom/elemreduce.hpp"
#include "syxg50/mu_rom/model.hpp"

namespace hvyxg::syxg50::mu_rom {

enum class Engine : std::uint8_t { Mu1000, Mu2000, Mu128 };

/// decMU1000.MU1000_Program / MU90_Waverom: two 16-bit halves of a 32-bit bus.
Bytes interleave16(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b);
/// utils.Dejumble: swap the bytes of every 16-bit word.
Bytes dejumble(std::span<const std::uint8_t> in);
/// decMU1000.MU2000_Flash_Split: S-MU2000's mu2000_flash.bin → (high, low).
std::pair<Bytes, Bytes> mu2000_flash_split(std::span<const std::uint8_t> flash);
/// decMU1000.MU2000_Scaling: per-note scaling tables back to break points (in place).
void mu2000_scaling(Bytes& image);

/// decBase.Create_Table for one voice map of the dejumbled program image.
Table create_table(Engine engine, const Bytes& image, bool basic, std::span<const std::uint8_t> waverom,
                   elemreduce::EnvIds& env_ids);

} // namespace hvyxg::syxg50::mu_rom
