// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// SXG-Create-NF MU table loader (`enable_mu`): big and classic layouts, the MAP2 voice-map
// appendix, and the patched DLL's lookups on the loaded bank.
// Input: [seed] [layout / voice map] [u24 bin length] [bin] [wave].
#include <cstddef>
#include <cstdint>

#include "fuzz_support.hpp"
#include "hvyxg/types.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::Input in(data, size);
    const std::uint8_t seed = in.u8();
    const std::uint8_t mode = in.u8();
    const std::uint32_t bin_len = in.u8() | in.u8() << 8 | in.u8() << 16;
    const auto bin = in.take(bin_len);
    const auto wave = in.rest();
    hvyxg::BankOptions bo;
    bo.tables = fuzz::need_tables();
    bo.enable_mu = true;
    bo.mu_layout = static_cast<hvyxg::MuLayout>(mode % 3);
    bo.mu_voice_map = static_cast<hvyxg::MuVoiceMap>((mode >> 2) % 3);
    try {
        const auto bank = hvyxg::load_bank(std::vector<std::uint8_t>(bin.begin(), bin.end()),
                                           std::vector<std::uint8_t>(wave.begin(), wave.end()),
                                           hvyxg::Model::Auto, bo);
        fuzz::probe_bank(bank, seed);
    } catch (const hvyxg::BankError&) {
    }
    return 0;
}
