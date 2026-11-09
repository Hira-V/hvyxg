// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// Small public types shared by the public API and the engine: engine model, sample
// interpolation, reset mode and the bank loading error.

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace hvyxg {

struct EngineTables;

/// Which Yamaha engine a bank belongs to. `Auto` sniffs the bin table header.
enum class Model : std::uint8_t { Auto, Syxg50, Syxg2006le };

/// Sample interpolation. `Linear` is what both DLLs do; the others are quality options.
enum class Interpolation : std::uint8_t { Linear, Hermite, Lanczos3 };

/// Table layout of an SXG-Create-NF MU bank (docs/ENGINE.md, "MU banks"): `Classic` is the
/// S-YXG50's own layout (MU50 / MU80 / MU90, the "Enhanced" patched DLL), `Big` the extended
/// one (MU100 / MU128 / MU1000 / MU2000, the "Full" patched DLL). `Auto` reads it from the
/// table.
enum class MuLayout : std::uint8_t { Auto, Classic, Big };

/// Voice map of an MU100 / MU128 / MU1000 bank: `Default` is the one the table names (its
/// voice-map appendix), `Native` the revoiced map, `Basic` the MU90-compatible one.
enum class MuVoiceMap : std::uint8_t { Default, Native, Basic };

/// The MU model of a ROM set for the built-in converter (hvyxg/mu_rom.hpp): MU1000 v2.01 and
/// MU2000 v2.01 (verified), MU128 v2.00 (untested). `Auto` picks the first complete set in
/// that order.
enum class MuRomModel : std::uint8_t { Auto, Mu1000, Mu2000, Mu128 };

/// How a bank is loaded beyond its model (`load_bank`).
struct BankOptions {
    /// The engine tables the bank's engine plays with (hvyxg/engine_tables.hpp): required, and
    /// ready for the bank's model (the loaders throw BankError otherwise).
    std::shared_ptr<const EngineTables> tables;
    /// Load an SXG-Create-NF MU bank and play it as the SXG-Create-NF patched S-YXG50 does.
    bool enable_mu = false;
    MuLayout mu_layout = MuLayout::Auto;
    MuVoiceMap mu_voice_map = MuVoiceMap::Default;
};

/// The mode the last reset message selected (the tone modules map banks by it).
/// Power-on is XG, as in both DLLs.
enum class ResetMode : std::uint8_t { XG, GM, GM2, GS };

/// What failed in loading: a missing or unreadable file, a bad header, a bad table.
enum class ErrorKind : std::uint8_t { Io, Header, Table };

/// An error of the non-throwing API (what a BankError carries in the throwing one).
struct Error {
    ErrorKind kind = ErrorKind::Io;
    std::string message;
};

/// Bank loading failed: missing or unreadable file, short file, bad header, bad table.
class BankError : public std::runtime_error {
public:
    using Kind = ErrorKind;
    BankError(Kind kind, const std::string& what) : std::runtime_error(what), kind_(kind) {}
    explicit BankError(const Error& e) : BankError(e.kind, e.message) {}
    Kind kind() const { return kind_; }

private:
    Kind kind_;
};

} // namespace hvyxg
