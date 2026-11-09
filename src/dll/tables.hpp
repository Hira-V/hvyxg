// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The table descriptors behind EngineTables (hvyxg/engine_tables.hpp). hvyxg ships no table
// data and no DLL addresses: each table is a descriptor (its element type in the DLL, its
// length, a 64-bit rolling hash and an xxHash3-128 of its bytes), and load_engine_tables scans
// the data sections of a DLL image for a window with that rolling hash, verifies it by the
// xxHash3-128 and converts it into the table's place in the EngineTables object.

#include <cstddef>
#include <cstdint>
#include <span>

namespace hvyxg {
struct EngineTables;
}

namespace hvyxg::table_store {

/// Element types, in the DLL and in hvyxg's storage.
enum class Elem : std::uint8_t { U8, S8, U16, S16, U32, S32, F32 };

/// Which engines need a table.
enum Need : unsigned { kSyxg50 = 1, kSyxg2006le = 2, kBoth = 3 };

/// One way to fill a table. Several descriptors may fill the same table (alternatives found in
/// different DLLs); the table is there once any of them is found.
struct TableDesc {
    const char* name;
    Elem in_dll;        ///< element type as the DLL stores it
    Elem stored;        ///< element type of the table (values are converted, range-checked)
    std::uint32_t count;
    std::uint64_t roll;  ///< rolling_hash of the DLL bytes
    std::uint64_t xxh_high, xxh_low;  ///< XXH3_128bits of the DLL bytes
    void* (*dest)(EngineTables& tables);  ///< the table's first element in `tables`
    unsigned need;       ///< Need bits
    void (*fix)(void* dest) = nullptr;  ///< applied after this alternative is stored
};

/// The descriptor lists of the table files (generated).
std::span<const TableDesc> fx_tables();
std::span<const TableDesc> le_voice_tables();
std::span<const TableDesc> syxg50_voice_tables();

/// The rolling hash of the pre-filter: Σ b[i]·B^(n−1−i) mod 2^64, B = 0x100000001b3.
std::uint64_t rolling_hash(std::span<const std::uint8_t> bytes);

} // namespace hvyxg::table_store
