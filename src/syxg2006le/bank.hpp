// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "bank_impl.hpp"

namespace hvyxg::syxg2006le {

/// Load the voice table and the (scrambled) wave table. Throws `BankError`.
/// nullptr and `error` when the tables are malformed.
std::shared_ptr<const Bank> load_bank(const BankMemory& memory, const BankOptions& options,
                                      std::shared_ptr<const void> owned, Error& error);

} // namespace hvyxg::syxg2006le
