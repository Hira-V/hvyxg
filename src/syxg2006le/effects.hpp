// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// System effects (reverb, chorus, variation + the effect mixer) of the modelled DLL.

#include <memory>

#include "module.hpp"

namespace hvyxg::syxg2006le {

std::unique_ptr<SystemEffects> make_system_effects(const ModuleContext& ctx);

} // namespace hvyxg::syxg2006le
