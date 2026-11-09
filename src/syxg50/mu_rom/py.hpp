// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Translated from SXG-Create-NF, Copyright (c) 2026 Soundshock, BSD-3-Clause (THIRD_PARTY.md)
#pragma once
// Helpers that reproduce the Python semantics SXG-Create-NF's converter relies on, so the C++
// port produces byte-identical tables (THIRD_PARTY.md):
// insertion-ordered dicts, `round()` (half to even, and to n decimals), the compensated
// `sum()` of Python 3.12, floor division, and correctly rounded integer true division.
// Floating-point expressions in the converter are written in Python's evaluation order and
// compiled without FMA contraction (CMakeLists.txt).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "fail.hpp"

// 128-bit integers (Python's big ints): the compiler's __int128, or, for a target without it, a
// host's own types with the same operators: HVYXG_INT128_HEADER names their header, HVYXG_I128
// and HVYXG_U128 the types (a build for Hira-V's RV32 cores gives its libu bigint classes). The
// code converts to and from other types explicitly, as such classes require.
#if defined(HVYXG_INT128_HEADER)
#include HVYXG_INT128_HEADER
#endif

namespace hvyxg::syxg50::mu_rom {

#if defined(HVYXG_INT128_HEADER)
using i128 = HVYXG_I128;
using u128 = HVYXG_U128;
#else
__extension__ typedef __int128 i128;
__extension__ typedef unsigned __int128 u128;
#endif

/// A failed `assert` / exception of the Python converter.
struct ConvertError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline void check(bool ok, const char* what) {
    (void)what;  // unused without exceptions
    if (!ok) HVYXG_THROW(ConvertError(what));
}

struct I128Hash {
    std::size_t operator()(i128 v) const noexcept {
        const auto lo = static_cast<std::uint64_t>(v), hi = static_cast<std::uint64_t>(v >> 64);
        return std::hash<std::uint64_t>{}(lo ^ (hi * 0x9E3779B97F4A7C15ull));
    }
};

/// Python dict: insertion order, assignment to an existing key keeps its place, deletion
/// leaves a hole (re-insertion goes to the end).
template <class K, class V, class H = std::hash<K>>
class ODict {
public:
    bool contains(const K& k) const { return idx_.count(k) != 0; }
    V* find(const K& k) {
        const auto it = idx_.find(k);
        return it == idx_.end() ? nullptr : &items_[it->second]->second;
    }
    const V* find(const K& k) const {
        const auto it = idx_.find(k);
        return it == idx_.end() ? nullptr : &items_[it->second]->second;
    }
    V& at(const K& k) {
        V* v = find(k);
        check(v != nullptr, "dict key missing");
        return *v;
    }
    const V& at(const K& k) const {
        const V* v = find(k);
        check(v != nullptr, "dict key missing");
        return *v;
    }
    void set(const K& k, V v) {
        const auto it = idx_.find(k);
        if (it != idx_.end()) {
            items_[it->second]->second = std::move(v);
            return;
        }
        idx_.emplace(k, items_.size());
        items_.emplace_back(std::in_place, k, std::move(v));
        ++size_;
    }
    void erase(const K& k) {
        const auto it = idx_.find(k);
        if (it == idx_.end()) return;
        items_[it->second].reset();
        idx_.erase(it);
        --size_;
    }
    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    /// Visit the entries in order (f(key, value&)); entries added while visiting are visited
    /// too only if `live` (Python would raise; the converter never does that).
    template <class F>
    void each(F&& f) {
        const std::size_t n = items_.size();
        for (std::size_t i = 0; i < n; ++i)
            if (items_[i]) f(items_[i]->first, items_[i]->second);
    }
    template <class F>
    void each(F&& f) const {
        for (const auto& it : items_)
            if (it) f(it->first, it->second);
    }
    std::vector<K> keys() const {
        std::vector<K> out;
        out.reserve(size_);
        for (const auto& it : items_)
            if (it) out.push_back(it->first);
        return out;
    }
    std::vector<V> values() const {
        std::vector<V> out;
        out.reserve(size_);
        for (const auto& it : items_)
            if (it) out.push_back(it->second);
        return out;
    }

private:
    std::vector<std::optional<std::pair<K, V>>> items_;
    std::unordered_map<K, std::size_t, H> idx_;
    std::size_t size_ = 0;
};

/// Python `round(x)` for a float: half to even.
inline std::int64_t py_round(double x) { return static_cast<std::int64_t>(std::nearbyint(x)); }

/// Python `round(x, n)`: correctly rounded to n decimals (the C library's "%.nf" rounds the
/// exact binary value, as Python's dtoa does).
inline double py_round_nd(double x, int n) {
    char buf[512];
    std::snprintf(buf, sizeof buf, "%.*f", n, x);
    return std::strtod(buf, nullptr);
}

/// Python 3.12 `sum()` of floats (start 0): Neumaier compensated summation.
inline double py_sum(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double f = 0.0 + v[0], c = 0.0;
    for (std::size_t i = 1; i < v.size(); ++i) {
        const double x = v[i];
        const double t = f + x;
        if (std::fabs(f) >= std::fabs(x)) c += (f - t) + x;
        else c += (x - t) + f;
        f = t;
    }
    if (c != 0.0 && std::isfinite(c)) f += c;
    return f;
}

/// Python `a // b` and `a % b` for integers (floor).
inline std::int64_t floordiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}
inline std::int64_t ceildiv(std::int64_t a, std::int64_t b) { return -floordiv(-a, b); }

/// Python `a / b` for non-negative integers: correctly rounded, also above 2^53.
inline double py_truediv(u128 a, std::uint64_t b) {
    if (b == 0) HVYXG_THROW(ConvertError("division by zero"));
    if (a < (static_cast<u128>(1) << 53) && b < (std::uint64_t{1} << 53))
        return static_cast<double>(static_cast<std::uint64_t>(a)) / static_cast<double>(b);
    // Scale so the quotient has at least 64 significant bits, fold the remainder into a
    // sticky bit, and let the (correctly rounding) integer → double conversion round once.
    const auto bitlen = [](u128 x) {
        int n = 0;
        while (x) {
            ++n;
            x >>= 1;
        }
        return n;
    };
    const int shift = std::max(0, 65 - (bitlen(a) - bitlen(b)));
    check(bitlen(a) + shift <= 127, "true division out of range");
    const u128 num = a << shift;
    u128 q = num / b;
    bool sticky = (num % b) != 0;
    int up = 0;
    while (q >> 64) {
        sticky = sticky || (q & 1);
        q >>= 1;
        ++up;
    }
    std::uint64_t q64 = static_cast<std::uint64_t>(q);
    if (sticky) q64 |= 1;
    return std::ldexp(static_cast<double>(q64), up - shift);
}

inline std::string to_str(std::int64_t v) { return std::to_string(v); }
inline std::string to_str(i128 v) {
    if (v >= i128(INT64_MIN) && v <= i128(INT64_MAX)) return std::to_string(static_cast<std::int64_t>(v));
    const bool neg = v < i128(0);
    u128 u = neg ? static_cast<u128>(-v) : static_cast<u128>(v);
    std::string s;
    while (u) {
        s.insert(s.begin(), static_cast<char>('0' + static_cast<int>(u % 10)));
        u /= 10;
    }
    return neg ? "-" + s : s;
}

/// zlib CRC-32.
std::uint32_t crc32(const std::uint8_t* p, std::size_t n, std::uint32_t crc = 0);

} // namespace hvyxg::syxg50::mu_rom
