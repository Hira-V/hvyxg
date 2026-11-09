// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// Loading a sound bank (its bin / voice and wave tables, from files or the caller's memory).

#include <cstdint>
#if !HVYXG_NO_FILES
#include <filesystem>
#endif
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "hvyxg/types.hpp"

namespace hvyxg {

class BankImpl;

/// A loaded bank: its tables (or the caller's memory they lie in, BankMemory) and the engine
/// that plays them. Immutable once loaded; one bank serves any number of synths on any threads.
/// Banks are made by `load_bank` / `load_mu_roms` only.
class Bank {
public:
    virtual ~Bank();
    Bank(const Bank&) = delete;
    Bank& operator=(const Bank&) = delete;

    /// The engine: S-YXG50 (also for SXG-Create-NF MU banks) or S-YXG2006LE.
    virtual Model type() const = 0;
    /// An SXG-Create-NF MU bank played with the patched S-YXG50's behaviour (`enable_mu`).
    virtual bool mu() const { return false; }
    /// Voices of the modelled DLL (S-YXG50: 128 register sets, 2006LE: 32 DSP voices).
    virtual std::uint16_t default_polyphony() const = 0;
    /// The engine tables the bank plays with (BankOptions::tables).
    const EngineTables* tables() const { return tables_.get(); }

private:
    friend class BankImpl;  // every bank is one (src/bank_impl.hpp)
    Bank() = default;
    std::shared_ptr<const EngineTables> tables_;
    /// The memory the bank reads, when it owns it (the vector / file overloads of load_bank).
    std::shared_ptr<const void> owned_;
};

/// Bank files in memory the caller owns: each buffer holds a file's bytes followed by `kPad`
/// spare bytes. Loading changes the buffers in place (the wave descrambled or decrypted, the
/// spare bytes written) and the bank reads them from there: they must stay valid and unchanged
/// while the bank, or any synth playing it, exists. One such bank serves any number of synths.
/// The S-YXG2006LE wave buffer must be 2-byte aligned (its samples are read as 16-bit words).
struct BankMemory {
    static constexpr std::size_t kPad = 8;
    std::span<std::uint8_t> bin;   ///< the bin (S-YXG50) / voice (S-YXG2006LE) table, then kPad bytes
    std::span<std::uint8_t> wave;  ///< the wave / data table, then kPad bytes; empty when the bin holds it
};

/// The engine model of a bin table from its header (bank_io.cpp): "UTG VPRM" = S-YXG2006LE,
/// a valid S-YXG50 segment header = S-YXG50, else nullopt.
std::optional<Model> sniff_model(std::span<const std::uint8_t> bin);

// ---- Without exceptions or files ------------------------------------------------------------

/// Load a bank over the caller's memory (BankMemory). `Auto` sniffs the bin header. nullptr and
/// `error` when the tables are malformed or the engine tables lack what the bank's model needs.
std::shared_ptr<const Bank> load_bank(const BankMemory& memory, Model model, const BankOptions& options,
                                      Error& error) noexcept;
/// Load a bank from its bin (voice) and data (wave) tables, which the bank then owns.
std::shared_ptr<const Bank> load_bank(std::vector<std::uint8_t> bin, std::vector<std::uint8_t> data, Model model,
                                      const BankOptions& options, Error& error) noexcept;

#if !HVYXG_NO_FILES
// ---- Throwing `BankError`, and from files ----------------------------------------------------

/// Load a bank from its bin (voice) table and data (wave) table. `Auto` sniffs the bin
/// header. Throws `BankError`.
std::shared_ptr<const Bank> load_bank(const std::filesystem::path& bin_file,
                                      const std::filesystem::path& data_file,
                                      Model model = Model::Auto, const BankOptions& options = {});
std::shared_ptr<const Bank> load_bank(std::vector<std::uint8_t> bin, std::vector<std::uint8_t> data,
                                      Model model = Model::Auto, const BankOptions& options = {});
/// Load a bank over the caller's memory (BankMemory). Throws `BankError`.
std::shared_ptr<const Bank> load_bank(const BankMemory& memory, Model model = Model::Auto,
                                      const BankOptions& options = {});
#endif

} // namespace hvyxg
