// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The engine side of a bank (internal): what the front end, the renderer and the tone modules
// ask of it. Every hvyxg::Bank is a BankImpl; impl() gets it back.
//
// Each Yamaha engine has its own bank format and its own voice architecture, so a bank is
// not a neutral list of samples: it builds the tone module and system effects that model its
// DLL.

#include <cstdint>
#include <memory>
#include <optional>

#include "hvyxg/bank.hpp"
#include "midi/xg_ram.hpp"
#include "module.hpp"

namespace hvyxg {

/// Internal bank MSB for "the GS / GM drum kit" (drum-mode parts whose bank MSB names no kit).
inline constexpr std::uint8_t kDrumBankMsbGs = 0x7B;
inline constexpr std::uint8_t kDrumBankMsbGm2 = 0x78;
inline constexpr std::uint8_t kSfxBankMsbXg = 0x7E;
inline constexpr std::uint8_t kDrumBankMsbXg = 0x7F;

using midi::xg::DrumKitSetup;

class BankImpl : public Bank {
public:
    BankImpl(std::shared_ptr<const EngineTables> tables, std::shared_ptr<const void> owned) {
        tables_ = std::move(tables);
        owned_ = std::move(owned);
    }

    /// The drum part's default XG note assign (08 pp 06) in this engine after a reset of
    /// `mode` (0 single, 1 multi, 2 per drum key).
    virtual std::uint8_t drum_part_key_assign(ResetMode mode) const = 0;
    /// Whether XG parameter changes / bulk dumps are obeyed after a GS reset (the 2006LE
    /// ignores them until XG System On [meas]; the S-YXG50 obeys them).
    virtual bool xg_parameters_in_gs_mode() const { return true; }
    /// Whether XG messages go through the S-YXG50's parameter table: a parameter change
    /// carries one whole parameter (a lone byte of a multi-byte one such as the part detune
    /// `08 nn 09/0A` does nothing), bulk dumps write whole parameters up to the end of the
    /// table block and are not checksum-checked [code, meas]. Otherwise (2006LE) each data
    /// byte of a parameter change is written to its own address.
    virtual bool xg_parameter_table() const { return false; }
    /// The bank MSBs GS mode plays through, when the DLL implements GS by translating it onto
    /// XG banks: the 2006LE plays GS melodic programs from MSB 119 with the GS variation
    /// number (CC#0) as LSB, and GS drum parts from MSB 118 [code: its GS layer]. nullopt: GS
    /// has its own voice / kit tables (the S-YXG50).
    struct GsBanks {
        std::uint8_t melodic_msb;
        std::uint8_t drum_msb;
    };
    virtual std::optional<GsBanks> gs_banks() const { return std::nullopt; }

    /// A bank MSB that selects drum kits (XG 127, SFX 126, GM2 120, the GS marker; the
    /// 2006LE also 118).
    virtual bool is_kit_bank(std::uint8_t msb) const {
        msb &= 0x7F;
        return msb == kDrumBankMsbXg || msb == kSfxBankMsbXg || msb == kDrumBankMsbGm2 ||
               msb == kDrumBankMsbGs;
    }

    /// The XG drum-setup RAM image (notes 13..91) of drum kit (bank MSB, program), or
    /// nullopt when that is not a kit. The tone module reads the drum-setup RAM as the
    /// kit's current setup, so this image must be what it expects to find there before
    /// any drum-setup SysEx.
    virtual std::optional<DrumKitSetup> drum_setup(std::uint8_t bank_msb,
                                                   std::uint8_t program) const = 0;

    virtual std::unique_ptr<ToneModule> make_tone_module(const ModuleContext& ctx) const = 0;
    virtual std::unique_ptr<SystemEffects> make_system_effects(const ModuleContext& ctx) const = 0;

};

/// The engine side of `bank` (every bank is a BankImpl).
inline const BankImpl& impl(const Bank& bank) { return static_cast<const BankImpl&>(bank); }
inline std::shared_ptr<const BankImpl> impl(std::shared_ptr<const Bank> bank) {
    return std::static_pointer_cast<const BankImpl>(std::move(bank));
}

} // namespace hvyxg
