// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Finding the bank by content (Synth::Options::bank_paths) and the check of banks embedded in
// a DLL (bank_search.hpp).
#include <doctest/doctest.h>

#include "bank_search.hpp"
#include "bank_io.hpp"
#include "test_support.hpp"

using namespace hvyxg;

TEST_CASE("bank search: a stock bank embedded under its stock name must be the stock bank") {
    PluginTables t;
    t.bin_name = "SXGBIN41.TBL";
    t.bin = std::vector<std::uint8_t>(64, 0x55);
    t.wave_name = "SXGWAVE4.TBL";
    t.wave = std::vector<std::uint8_t>(64, 0xAA);
    CHECK_THROWS_AS(bank_search::check_embedded(t), BankError);
    t.bin_name = "sxgbnw6l.tbl";  // any case
    CHECK_THROWS_AS(bank_search::check_embedded(t), BankError);
    t.bin_name = "SXGMU1KX.TBL";  // an SXG-Create-NF conversion: accepted as it is
    CHECK_NOTHROW(bank_search::check_embedded(t));
    const auto root = test::asset_root();
    if (!root || !std::filesystem::exists(*root / "roms/yxg50/sxgbin41.tbl")) return;
    t.bin_name = "SXGBIN41.TBL";
    t.bin = read_file(*root / "roms/yxg50/sxgbin41.tbl", "bin");
    t.wave = read_file(*root / "roms/yxg50/sxgwave4.tbl", "wave");
    CHECK_NOTHROW(bank_search::check_embedded(t));
    t.wave[100] ^= 1;
    CHECK_THROWS_AS(bank_search::check_embedded(t), BankError);
}

TEST_CASE("bank search: banks are found by content and chosen by model") {
    HVYXG_NEED_TABLES();
    const auto root = test::asset_root();
    if (!root || !std::filesystem::is_directory(*root / "roms/yxg50") || !std::filesystem::is_directory(*root / "roms/yxg2006le")) {
        MESSAGE("skipped: needs roms/yxg50 and roms/yxg2006le");
        return;
    }
    const auto roms = *root / "roms";
    const auto req = [](std::filesystem::path p, Model m = Model::Auto) {
        bank_search::Request r;
        r.paths = {std::move(p)};
        r.model = m;
        r.bank_options = test::bank_options();
        return r;
    };
    // The 4 MB and 2 MB stock tables in one folder: the 4 MB bank, as the DLL defaults.
    auto b = bank_search::load(req(roms / "yxg50"));
    CHECK(b->type() == Model::Syxg50);
    // Both engines' stock banks: a choice the caller has to make.
    CHECK_THROWS_AS(bank_search::load(req(roms)), BankError);
    CHECK(bank_search::load(req(roms, Model::Syxg50))->type() == Model::Syxg50);
    CHECK(bank_search::load(req(roms, Model::Syxg2006le))->type() == Model::Syxg2006le);
    // Nothing there.
    CHECK_THROWS_AS(bank_search::load(req(roms / "no such folder")), BankError);
}
