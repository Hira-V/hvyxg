// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The S-YXG2006LE voice table header: "UTG VPRM" (the full text is
// "UTG VPRM 06 07 28 15 28\0").

#include <algorithm>
#include <cstdint>
#include <span>

namespace hvyxg::syxg2006le::tbl {

inline bool check_header(std::span<const std::uint8_t> file) {
    static constexpr char kMagic[] = "UTG VPRM";
    return file.size() >= 8 && std::equal(file.begin(), file.begin() + 8, kMagic);
}

} // namespace hvyxg::syxg2006le::tbl
