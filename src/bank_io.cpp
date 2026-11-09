// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "bank_io.hpp"

#include <fstream>

#include "hvyxg/types.hpp"

namespace hvyxg {

std::vector<std::uint8_t> read_file(const std::filesystem::path& path, const std::string& what) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
        throw BankError(BankError::Kind::Io, what + ": no such file: " + path.string());
    std::ifstream in(path, std::ios::binary);
    if (!in) throw BankError(BankError::Kind::Io, what + ": cannot open " + path.string());
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) throw BankError(BankError::Kind::Io, what + ": cannot read " + path.string());
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(data.data()), size))
        throw BankError(BankError::Kind::Io, what + ": read error in " + path.string());
    return data;
}

} // namespace hvyxg
