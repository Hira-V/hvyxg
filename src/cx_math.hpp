// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// Compile-time maths: ln, log10, exp, 10^x, sin, cos, tan and the rounding helpers, all
// constexpr and evaluated by the compiler in IEEE double (no dependence on the C library's
// <cmath>), and table_hash(), which pins a table built at compile time (the DPCM table) to
// the values it must have.
//
// Ranges: log() for x > 0, exp() for |x| < 700, sin() / cos() / tan() for |x| <= π/2.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace hvyxg::cx {

inline constexpr double kPi = 3.14159265358979323846264338327950288;
inline constexpr double kLn2 = 0.693147180559945309417232121458176568;
inline constexpr double kLn10 = 2.30258509299404568401799145468436421;

/// Natural log: x = m · 2^e with m in [√½, √2), ln m = 2·atanh((m − 1) / (m + 1)).
constexpr double log(double x) {
    int e = 0;
    while (x > 1.4142135623730951) { x *= 0.5; ++e; }
    while (x < 0.7071067811865476) { x *= 2.0; --e; }
    const double s = (x - 1.0) / (x + 1.0);
    const double s2 = s * s;
    double term = s;
    double sum = 0.0;
    for (int k = 1; k < 61; k += 2) {
        sum += term / k;
        term *= s2;
    }
    return 2.0 * sum + e * kLn2;
}

constexpr double log10(double x) { return log(x) / kLn10; }

/// e^x: x = k·ln 2 + r with |r| <= ln 2 / 2, Taylor series in r, then scaled by 2^k.
constexpr double exp(double x) {
    const int k = static_cast<int>(x / kLn2 + (x < 0 ? -0.5 : 0.5));
    const double r = x - k * kLn2;
    double term = 1.0;
    double sum = 1.0;
    for (int n = 1; n < 30; ++n) {
        term *= r / n;
        sum += term;
    }
    for (int i = 0; i < k; ++i) sum *= 2.0;
    for (int i = 0; i > k; --i) sum *= 0.5;
    return sum;
}

constexpr double pow10(double x) { return exp(x * kLn10); }

constexpr double sin(double x) {
    const double x2 = x * x;
    double term = x;
    double sum = x;
    for (int n = 1; n < 30; ++n) {
        term *= -x2 / ((2.0 * n) * (2.0 * n + 1.0));
        sum += term;
    }
    return sum;
}

constexpr double cos(double x) {
    const double x2 = x * x;
    double term = 1.0;
    double sum = 1.0;
    for (int n = 1; n < 30; ++n) {
        term *= -x2 / ((2.0 * n - 1.0) * (2.0 * n));
        sum += term;
    }
    return sum;
}

constexpr double tan(double x) { return sin(x) / cos(x); }

constexpr long long floor_int(double x) {
    const auto t = static_cast<long long>(x);
    return (x < 0 && static_cast<double>(t) != x) ? t - 1 : t;
}
constexpr long long ceil_int(double x) { return -floor_int(-x); }
/// Round half away from zero.
constexpr long long round_int(double x) { return x < 0 ? -floor_int(-x + 0.5) : floor_int(x + 0.5); }

/// FNV-1a over a table's values (integers as 64-bit two's complement, floats as their 32-bit
/// pattern), little-endian.
template <class T, std::size_t N>
constexpr std::uint64_t table_hash(const std::array<T, N>& a) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const T& x : a) {
        std::uint64_t v = 0;
        if constexpr (std::is_floating_point_v<T>)
            v = std::bit_cast<std::uint32_t>(static_cast<float>(x));
        else
            v = static_cast<std::uint64_t>(static_cast<std::int64_t>(x));
        for (int b = 0; b < 8; ++b) {
            h ^= (v >> (8 * b)) & 0xffu;
            h *= 0x100000001b3ull;
        }
    }
    return h;
}

} // namespace hvyxg::cx
