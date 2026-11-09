// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// Helpers shared by the bank loaders: whole-file reading and the wave-table descrambler both
// Yamaha engines use.

#include <cstdint>
#if !HVYXG_NO_FILES
#include <filesystem>
#endif
#include <span>
#include <string>
#include <vector>

namespace hvyxg {

#if !HVYXG_NO_FILES
/// Read a whole file. Throws `BankError` (Io) naming `what` and the path (hosted builds).
std::vector<std::uint8_t> read_file(const std::filesystem::path& path, const std::string& what);
#endif

/// The wave descrambler (S-YXG50 key 0x5D, S-YXG2006LE key 0x5B): for byte i,
/// `x = b ^ key ^ (i & 0xFF)`, swap the nibbles of `x`, then invert the key (bank.cpp).
void descramble_wave(std::uint8_t key, std::span<std::uint8_t> data);

} // namespace hvyxg
