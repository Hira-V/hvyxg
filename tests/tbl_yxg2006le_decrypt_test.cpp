// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// S-YXG2006LE wave table decryption and sample decoding (the DLL's format; see
// src/syxg2006le/tbl/decrypt.hpp).

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "bank_io.hpp"
#include "syxg2006le/tbl/decrypt.hpp"
#include "test_support.hpp"

using hvyxg::descramble_wave;
namespace yxg2006le = hvyxg::syxg2006le::tbl;

namespace {

/// The DLL's loader followed by the unmasking when a sample is read (0x5A
/// per byte), step by step as the DLL does them; `decrypt` must produce the same bytes.
std::vector<std::uint8_t> dll_load_then_unmask(std::vector<std::uint8_t> d) {
    descramble_wave(0x5B, d); // with key 0x5b
    std::uint8_t saved = 0;
    for (std::size_t i = 0; i < d.size(); ++i) {
        if (i % 2 == 0) {
            saved = d[i] ^ 0x5A;
        } else {
            d[i - 1] = d[i] ^ 0x5A;
            d[i] = saved;
        }
    }
    for (std::size_t i = 0; i + 1 < d.size(); ++i) d[i] ^= 0x5A; // the mask removed when a sample is read
    if (d.size() % 2 == 0 && !d.empty()) d.back() ^= 0x5A;
    return d;
}

} // namespace

TEST_CASE("yxg2006le_decrypt_matches_dll_pipeline") {
    for (const std::size_t n : {0u, 1u, 2u, 7u, 256u, 1001u}) {
        std::vector<std::uint8_t> raw(n);
        for (std::size_t i = 0; i < n; ++i) raw[i] = static_cast<std::uint8_t>(i * 37 + 11);
        auto ours = raw;
        yxg2006le::decrypt(ours);
        CHECK(ours == dll_load_then_unmask(raw));
    }
}

TEST_CASE("yxg2006le_unpack12") {
    // pair (0x123, -0x456 as 12-bit 0xBAA): left-justified int16 0x1230 and 0xBAA0
    const std::vector<std::uint8_t> d = {0x23, 0xA1, 0xBA};
    CHECK(yxg2006le::unpack12(d, 0) == static_cast<std::int16_t>(0x1230));
    CHECK(yxg2006le::unpack12(d, 1) == static_cast<std::int16_t>(0xBAA0));
}

TEST_CASE("yxg2006le_compand8_table") {
    using yxg2006le::kCompand8;
    CHECK(kCompand8[0] == 0);
    CHECK(kCompand8[31] == 31);
    CHECK(kCompand8[32] == 32);
    CHECK(kCompand8[33] == 34);
    CHECK(kCompand8[64] == 96);
    CHECK(kCompand8[96] == 224);
    CHECK(kCompand8[127] == 472);
    CHECK(kCompand8[128] == 0);
    CHECK(kCompand8[255] == -472);
}

TEST_CASE("yxg2006le_decrypt_real_wave_table") {
    // Set HVYXG_2006LE_DATA to SXGDAT6L.TBL to run.
    const std::string path = test::asset("HVYXG_2006LE_DATA", "roms/yxg2006le/SXGDAT6L.TBL");
    if (path.empty()) {
        MESSAGE("HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> d{std::istreambuf_iterator<char>(in), {}};
    REQUIRE(d.size() > 8'000'002);
    yxg2006le::decrypt(d);
    // The table opens with a 16-bit wave rising from silence: 0, -40, -115, 191, 238, 364
    const auto s16 = [&](std::size_t i) { return static_cast<std::int16_t>(d[2 * i] | (d[2 * i + 1] << 8)); };
    CHECK(s16(0) == 0);
    CHECK(s16(1) == -40);
    CHECK(s16(2) == -115);
    CHECK(s16(3) == 191);
    // ...and a long 16-bit region is smooth audio (low second-difference energy).
    double diff = 0, energy = 0;
    for (std::size_t i = 3'000'001; i < 4'000'000; ++i) {
        const double x0 = s16(i - 1), x1 = s16(i), x2 = s16(i + 1);
        diff += (x2 - 2 * x1 + x0) * (x2 - 2 * x1 + x0);
        energy += x1 * x1;
    }
    CHECK(diff / energy < 0.1);
}
