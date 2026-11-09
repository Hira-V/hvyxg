// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Engine tables assembled from a law and windows read from the DLL: the whole table must be the
// DLL's table (the hashes are those of the complete tables, as the DLLs hold them).
#include <doctest/doctest.h>

#include "cx_math.hpp"
#include "syxg2006le/tbl/dll_tables.hpp"
#include "syxg50/dll_tables.hpp"
#include "test_support.hpp"

using hvyxg::cx::table_hash;

TEST_CASE("dll tables: the EG time tables are the DLLs' (law plus the fast end)") {
    HVYXG_NEED_TABLES();
    CHECK(table_hash(test::tables()->syxg50.kDecayTime) == 0xec8879ab2f38fe46ull);
    CHECK(table_hash(test::tables()->syxg50.kReleaseTime) == 0x0885dc7c25a70890ull);
    CHECK(table_hash(test::tables()->le.kDecayTime) == 0x7f10b77425726967ull);
    CHECK(table_hash(test::tables()->le.kReleaseTime) == 0x43c42dc2b734ca1eull);
}
