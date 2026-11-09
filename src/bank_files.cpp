// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// The throwing, file-reading load_bank overloads (hosted builds), over bank.cpp's.
#include "bank_impl.hpp"
#include "bank_io.hpp"

namespace hvyxg {

std::shared_ptr<const Bank> load_bank(const BankMemory& memory, Model model, const BankOptions& options) {
    Error e;
    auto bank = load_bank(memory, model, options, e);
    if (!bank) throw BankError(e);
    return bank;
}

std::shared_ptr<const Bank> load_bank(std::vector<std::uint8_t> bin, std::vector<std::uint8_t> data,
                                      Model model, const BankOptions& options) {
    Error e;
    auto bank = load_bank(std::move(bin), std::move(data), model, options, e);
    if (!bank) throw BankError(e);
    return bank;
}

std::shared_ptr<const Bank> load_bank(const std::filesystem::path& bin_file,
                                      const std::filesystem::path& data_file, Model model,
                                      const BankOptions& options) {
    std::vector<std::uint8_t> bin = read_file(bin_file, "bin table");
    std::vector<std::uint8_t> data = read_file(data_file, "data table");
    return load_bank(std::move(bin), std::move(data), model, options);
}

} // namespace hvyxg
