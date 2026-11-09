// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Translated from SXG-Create-NF, Copyright (c) 2026 Soundshock, BSD-3-Clause (THIRD_PARTY.md)
// Ports of SXG-Create-NF utility/scaling.py, drumtrim.py and voicetrim.py (BSD-3-Clause,
// Copyright 2026 Soundshock). The arithmetic follows the Python expressions (see py.hpp).
#include "syxg50/mu_rom/trims.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "syxg50/mu_rom/elemreduce.hpp"
#include "syxg50/mu_rom/tables.hpp"
#include "fail.hpp"

namespace hvyxg::syxg50::mu_rom {

namespace {

constexpr std::array<int, 4> kDefaultBp{24, 48, 72, 96};

std::array<int, 128> table(const std::array<int, 4>& bp, const std::array<int, 4>& of) {
    std::array<int, 128> t{};
    const auto set = [&](int n, int v) {
        check(n >= 0 && n < 128, "scaling: note out of range");
        t[static_cast<std::size_t>(n)] = v;
    };
    for (int i = 0; i < 3; ++i) {
        const int a = bp[i], b = bp[i + 1];
        if (b <= a) continue;
        // math.trunc of the float quotient = C++ integer division for these small values
        for (int n = a; n <= b; ++n) set(n, of[i] + (of[i + 1] - of[i]) * (n - a) / (b - a));
    }
    for (int n = bp[3]; n < 128; ++n) set(n, of[3]);
    for (int n = 0; n <= bp[0]; ++n) set(n, of[0]);
    return t;
}

bool segment_ok(const std::array<int, 128>& t, int a, int b, std::optional<int> tb = std::nullopt) {
    const int tbv = tb ? *tb : t[static_cast<std::size_t>(b)];
    const int ta = t[static_cast<std::size_t>(a)], d = tbv - ta, n_ = b - a;
    for (int n = a + 1; n < std::min(b, 128); ++n) {
        const int q = d * (n - a);
        const int v = q >= 0 ? q / n_ : -((-q) / n_);
        if (t[static_cast<std::size_t>(n)] != ta + v) return false;
    }
    return true;
}

double level_db(int level) {
    for (const auto& s : kDrumLevelSteps)
        if (level >= s.low) return s.db;
    HVYXG_THROW(ConvertError("drum level below the lowest step"));
}

template <class T, std::size_t N>
const T* find_crc(const T (&tab)[N], std::uint32_t crc) {
    const auto it = std::lower_bound(std::begin(tab), std::end(tab), crc, [](const T& e, std::uint32_t c) { return e.crc < c; });
    return it != std::end(tab) && it->crc == crc ? it : nullptr;
}

} // namespace

std::array<int, 128> scaling_table(const std::uint8_t* b) {
    return table({b[0], b[1], b[2], b[3]}, {b[4] - 64, b[5] - 64, b[6] - 64, b[7] - 64});
}

std::tuple<std::array<int, 4>, std::array<int, 4>, int> scaling_inverse(const std::array<int, 128>& t) {
    const auto fits = [&](const std::array<int, 4>& bp, const std::array<int, 4>& of) { return table(bp, of) == t; };
    const auto at = [&](int x) { return t[static_cast<std::size_t>(x)]; };
    {
        const std::array<int, 4> of{at(24), at(48), at(72), at(96)};
        if (fits(kDefaultBp, of)) return {kDefaultBp, of, 0};
    }
    int lo = 0;
    while (lo < 127 && at(lo + 1) == at(0)) ++lo;
    int hi = 127;
    while (hi > 0 && at(hi - 1) == at(127)) --hi;
    if (hi <= lo) return {kDefaultBp, {at(0), at(0), at(0), at(0)}, 0};
    std::map<int, std::vector<int>> ends;
    const auto e = [&](int x) -> const std::vector<int>& {
        auto it = ends.find(x);
        if (it == ends.end()) {
            std::vector<int> v;
            for (int b = x + 1; b < 128; ++b)
                if (segment_ok(t, x, b)) v.push_back(b);
            it = ends.emplace(x, std::move(v)).first;
        }
        return it->second;
    };
    std::optional<std::pair<std::array<int, 4>, std::array<int, 4>>> best;
    int score = 1 << 30;
    const auto consider = [&](const std::array<int, 4>& bp, std::optional<std::array<int, 4>> of_in = std::nullopt) {
        const std::array<int, 4> of = of_in ? *of_in : std::array<int, 4>{at(bp[0]), at(bp[1]), at(bp[2]), at(bp[3])};
        int s = 0;
        for (int i = 0; i < 4; ++i) s += std::abs(bp[i] - kDefaultBp[i]);
        if (s < score && fits(bp, of)) {
            best = std::make_pair(bp, of);
            score = s;
        }
    };
    // Python visits every candidate; a candidate is only taken when its distance from the
    // default break points is below the best so far, so subtrees whose partial distance is
    // already at least that are skipped (same result, much faster).
    const auto dist = [&](int i, int v) { return std::abs(v - kDefaultBp[static_cast<std::size_t>(i)]); };
    for (int a = lo; a >= 0; --a) {
        if (dist(0, a) >= score) continue;
        for (const int b : std::vector<int>(e(a))) {
            if (dist(0, a) + dist(1, b) >= score) continue;
            if (b >= hi) consider({a, b, b, b});
            for (const int c : std::vector<int>(e(b))) {
                if (dist(0, a) + dist(1, b) + dist(2, c) >= score) continue;
                if (c >= hi) consider({a, b, c, c});
                for (const int d : std::vector<int>(e(c)))
                    if (d >= hi) consider({a, b, c, d});
            }
        }
    }
    if (best) return {best->first, best->second, 0};
    for (int a = lo; a >= 0; --a) {
        std::vector<int> bs = e(a);
        bs.push_back(a);
        for (const int b : bs)
            for (int v = -64; v < 64; ++v)
                if (segment_ok(t, b, 127, v)) consider({a, b, 127, 127}, std::array<int, 4>{at(a), at(b), v, at(127)});
    }
    if (best) return {best->first, best->second, 0};
    std::set<int> pset{0, 127, 24, 48, 72, 96};
    for (int n = 1; n < 127; ++n)
        if (at(n + 1) - at(n) != at(n) - at(n - 1)) pset.insert(n);
    const std::vector<int> pts(pset.begin(), pset.end());
    int err = 1 << 30;
    const std::size_t m = pts.size();
    for (std::size_t i0 = 0; i0 < m; ++i0)
        for (std::size_t i1 = i0 + 1; i1 < m; ++i1)
            for (std::size_t i2 = i1 + 1; i2 < m; ++i2)
                for (std::size_t i3 = i2 + 1; i3 < m; ++i3) {
                    const std::array<int, 4> bp{pts[i0], pts[i1], pts[i2], pts[i3]};
                    const std::array<int, 4> of{at(bp[0]), at(bp[1]), at(bp[2]), at(bp[3])};
                    const auto tt = table(bp, of);
                    int d = 0;
                    for (std::size_t k = 0; k < 128; ++k) d += tt[k] != t[k];
                    if (d < err) {
                        best = std::make_pair(bp, of);
                        err = d;
                    }
                }
    check(best.has_value(), "scaling: no fit");
    return {best->first, best->second, err};
}

std::pair<int, double> drum_trimmed_level(int level, std::span<const std::uint8_t> raw) {
    const CrcTrim* tr = find_crc(kDrumTrim, crc32(raw.data(), raw.size()));
    const double trim = (tr ? tr->db10 : 0) / 10.0;
    if (trim == 0.0 || !level) return {level, 0.0};
    const double target = level_db(level) + trim;
    int best = 1;
    double bk0 = 0;
    int bk1 = 0;
    for (int b = 1; b < 128; ++b) {
        const double k0 = py_round_nd(std::fabs(level_db(b) - target), 2);
        const int k1 = std::abs(b - level);
        if (b == 1 || k0 < bk0 || (k0 == bk0 && k1 < bk1)) {
            best = b;
            bk0 = k0;
            bk1 = k1;
        }
    }
    const double boost = target - level_db(best);
    return {best, (boost >= 0.3 && best >= kDrumLevelSteps[0].low) ? boost : 0.0};
}

std::uint32_t voice_key(std::span<const std::uint8_t> raw) {
    Bytes out(raw.begin(), raw.begin() + 14);
    const std::size_t n = (raw.size() - 14) / 84;
    for (std::size_t k = 0; k < n; ++k) {
        const std::uint8_t* e = raw.data() + 14 + 84 * k;
        const auto notes = [&](const std::uint8_t* b) {
            const auto t = scaling_table(b);
            for (const int x : t) out.push_back(static_cast<std::uint8_t>(x & 0xFF));
        };
        out.insert(out.end(), e, e + 36);
        notes(e + 36);
        out.insert(out.end(), e + 44, e + 58);
        notes(e + 58);
        out.insert(out.end(), e + 66, e + 84);
    }
    return crc32(out.data(), out.size());
}

std::pair<std::vector<int>, double> voice_trim_levels(std::uint32_t key, const std::vector<int>& levels) {
    const CrcTrim* tr = find_crc(kVoiceTrim, key);
    const double trim = (tr ? tr->db10 : 0) / 10.0;
    const bool any = std::any_of(levels.begin(), levels.end(), [](int l) { return l != 0; });
    if (trim == 0.0 || !any) return {levels, 0.0};
    const double f = std::pow(10.0, -trim / 40);
    std::vector<int> nw;
    for (const int l : levels) nw.push_back(l ? static_cast<int>(std::min<std::int64_t>(127, std::max<std::int64_t>(1, py_round(l * f)))) : 0);
    const int top = *std::max_element(levels.begin(), levels.end());
    const int mx = *std::max_element(nw.begin(), nw.end());
    const double missing = -trim - 40 * std::log10(static_cast<double>(mx) / top);
    return {nw, missing >= 0.3 ? py_round_nd(missing, 1) : 0.0};
}

void voice_key_trim(std::uint32_t key, Bytes& edata) {
    const CrcKeyTrim* tr = find_crc(kVoiceKeyTrim, key);
    if (tr == nullptr || !edata[57]) return;
    static constexpr int kKeys[4] = {30, 54, 78, 102};
    std::uint8_t offs[4];
    for (int i = 0; i < 4; ++i) {
        const double eff0 = elemreduce::effective_level(edata, kKeys[i]) * std::pow(10.0, -tr->db10[i] / 10.0 / 40);
        const double eff = eff0 < 145.0 ? eff0 : 145.0;
        offs[i] = static_cast<std::uint8_t>(std::max<std::int64_t>(
            0, std::min<std::int64_t>(127, py_round((eff - edata[57]) / elemreduce::kKsLevelSteps) + 64)));
    }
    for (int i = 0; i < 4; ++i) {
        edata[static_cast<std::size_t>(58 + i)] = static_cast<std::uint8_t>(kKeys[i]);
        edata[static_cast<std::size_t>(62 + i)] = offs[i];
    }
}

std::uint32_t crc32(const std::uint8_t* p, std::size_t n, std::uint32_t crc) {
    static constexpr auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

} // namespace hvyxg::syxg50::mu_rom
