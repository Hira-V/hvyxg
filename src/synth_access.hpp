// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// A synth's internals, for the tests (not part of the public API).

#include <cstddef>

#include "hvyxg/synth.hpp"
#include "midi/part.hpp"

namespace hvyxg {

struct SynthAccess {
    /// Part `i` as the front end holds it.
    static const midi::Part& part(const Synth& s, std::size_t i);
};

} // namespace hvyxg
