// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// S-YXG2006LE voice table (header, segments, voice / element records, block references) and
// wave decryption, then the tone module's lookups on the loaded bank.
// Input: [seed] [u24 bin length] [bin] [encrypted wave].
#include <cstddef>
#include <cstdint>

#include "fuzz_support.hpp"
#include "hvyxg/types.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::Input in(data, size);
    const std::uint8_t seed = in.u8();
    const std::uint32_t bin_len = in.u8() | in.u8() << 8 | in.u8() << 16;
    const auto bin = in.take(bin_len);
    const auto wave = in.rest();
    try {
        const auto bank = hvyxg::load_bank(std::vector<std::uint8_t>(bin.begin(), bin.end()),
                                           std::vector<std::uint8_t>(wave.begin(), wave.end()),
                                           (seed & 0x40) ? hvyxg::Model::Syxg2006le : hvyxg::Model::Auto,
                                           hvyxg::BankOptions{.tables = fuzz::need_tables()});
        fuzz::probe_bank(bank, seed);
    } catch (const hvyxg::BankError&) {
    }
    return 0;
}
