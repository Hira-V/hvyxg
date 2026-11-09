// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// hvyxg/version.hpp, generated from the CMake project version.
#include <doctest/doctest.h>

#include <string>

#include "hvyxg/version.hpp"

TEST_CASE("version: the generated header agrees with itself") {
    const std::string s = std::to_string(hvyxg::kVersionMajor) + "." + std::to_string(hvyxg::kVersionMinor) + "." +
                          std::to_string(hvyxg::kVersionPatch);
    CHECK(s == hvyxg::kVersionString);
    CHECK(hvyxg::kVersionMajor == HVYXG_VERSION_MAJOR);
}
