// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// System effects (reverb, chorus, variation + the effect mixer) of the modelled DLL.

#include <memory>

#include "module.hpp"

namespace hvyxg::syxg50 {

/// `mu`: the SXG-Create-NF patched DLL's effect behaviour (`--enable-mu`).
std::unique_ptr<SystemEffects> make_system_effects(const ModuleContext& ctx, bool mu = false);

} // namespace hvyxg::syxg50
