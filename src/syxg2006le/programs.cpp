// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "syxg2006le/programs.hpp"

#include "syxg2006le/bank_data.hpp"

namespace hvyxg::syxg2006le {

namespace {

/// hvyxg's internal "GS / GM drum kit" MSB plays the XG kits (MSB 127).
std::uint8_t xg_msb(std::uint8_t msb) { return msb == kDrumBankMsbGs ? kDrumBankMsbXg : msb; }

} // namespace

bool is_drum_bank(std::uint8_t msb) {
    msb = xg_msb(msb & 0x7F);
    return msb == kDrumBankMsbXg || msb == kSfxBankMsbXg || msb == kDrumBankMsbGm2 || msb == 118;
}

std::optional<std::uint16_t> resolve_voice(const BankData& bank, std::uint8_t msb,
                                           std::uint8_t lsb, std::uint8_t program,
                                           ResetMode mode) {
    msb = xg_msb(msb & 0x7F);
    lsb &= 0x7F;
    program &= 0x7F;
    const bool drums = is_drum_bank(msb);
    // GM mode ignores bank select (GM System On; measured: MSB 0 LSB 1 plays LSB 0).
    if (mode == ResetMode::GM && !drums) msb = lsb = 0;
    // Fallbacks measured with the DLL: a missing LSB plays LSB 0 of the same MSB (0/99 and
    // 121/30 → LSB 0), a missing program of a drum bank plays its program 0, and anything
    // else missing (MSB 1, MSB 5, MSB 64 program 2) is silent.
    if (auto v = bank.find_voice(msb, lsb, program)) return v;
    if (auto v = bank.find_voice(msb, 0, program)) return v;
    if (drums) return bank.find_voice(msb, 0, 0);
    return std::nullopt;
}

std::optional<std::uint16_t> drum_kit(const BankData& bank, std::uint8_t msb,
                                      std::uint8_t program) {
    const auto v = resolve_voice(bank, msb, 0, program);
    if (!v) return std::nullopt;
    const auto& voice = bank.voice(*v);
    if (!voice.is_drum() || voice.drum_kit == 0xFFFF) return std::nullopt;
    return voice.drum_kit;
}

DrumKitSetup kit_setup(const BankData& bank, std::uint16_t kit) {
    DrumKitSetup setup{};
    for (std::size_t i = 0; i < setup.size(); ++i) {
        if (const auto e = bank.table().drum_entry(kit, static_cast<std::uint8_t>(13 + i)))
            setup[i] = midi::xg::DrumSetup::from_bytes(*e);
    }
    return setup;
}

} // namespace hvyxg::syxg2006le
