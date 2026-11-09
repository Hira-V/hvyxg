// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// The kernels' portable references (kernels.hpp): the engines' own arithmetic, moved here unchanged.

#include "accel/kernels.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace hvyxg::accel::ref {

namespace {

// One sample at position i, for -pre ≤ i < loop_len on a walk that has not ended (the wave covers it:
// syxg50::ChipVoice::set_wave checks).
inline float direct(const Walk50& w, std::int32_t i) {
    if (w.pcm8) return static_cast<float>(static_cast<int>(w.wave[w.loop_start + static_cast<std::ptrdiff_t>(i)]) - 0x80) * (1.0f / 128.0f);
    const std::uint8_t* p = w.wave + w.loop_start + 2 * static_cast<std::ptrdiff_t>(i);
    return static_cast<float>(static_cast<int>(p[0] | p[1] << 8) - 0x8000) * (1.0f / 32768.0f);
}

// Any position: clamped below the pre-roll, wrapped past the loop end, 0 past the wave.
inline float sample_at(const Walk50& w, std::int32_t i) {
    if (i < -w.pre) i = -w.pre;
    else if (w.loop_len > 0 && i >= w.loop_len) i -= w.loop_len * ((i - w.loop_len) / w.loop_len + 1);
    if (w.pcm8) {
        const std::size_t at = w.loop_start + static_cast<std::size_t>(static_cast<std::ptrdiff_t>(i));
        return at < w.wave_size ? static_cast<float>(static_cast<int>(w.wave[at]) - 0x80) * (1.0f / 128.0f) : 0.0f;
    }
    const std::size_t at = w.loop_start + 2 * static_cast<std::size_t>(static_cast<std::ptrdiff_t>(i));
    if (at + 1 >= w.wave_size) return 0.0f;
    return static_cast<float>(static_cast<int>(w.wave[at] | w.wave[at + 1] << 8) - 0x8000) * (1.0f / 32768.0f);
}

template <std::size_t K>
void mix_fixed(const float* x, float* const* dst, const float* g, const float* dg, std::size_t n) {
    std::array<float, K> gg{};
    for (std::size_t b = 0; b < K; ++b) gg[b] = g[b];
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t b = 0; b < K; ++b) {
            dst[b][i] += x[i] * gg[b];
            gg[b] += dg[b];
        }
}

} // namespace

void resample50(Walk50& w, std::int32_t inc_int, std::uint32_t inc_frac, bool hermite, float* out, std::size_t n) {
    for (std::size_t k = 0; k < n; ++k) {
        if (w.ended) {
            out[k] = 0.0f;
            continue;
        }
        const float t = static_cast<float>(w.frac) * (1.0f / 4294967296.0f);
        const std::int32_t pos = w.pos;
        if (hermite) {
            float xm1, x0, x1, x2;
            if (pos - 1 >= -w.pre && pos + 2 < w.loop_len) {
                xm1 = direct(w, pos - 1), x0 = direct(w, pos), x1 = direct(w, pos + 1), x2 = direct(w, pos + 2);
            } else {
                xm1 = sample_at(w, pos - 1), x0 = sample_at(w, pos), x1 = sample_at(w, pos + 1), x2 = sample_at(w, pos + 2);
            }
            const float c1 = 0.5f * (x1 - xm1);
            const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
            const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            out[k] = ((c3 * t + c2) * t + c1) * t + x0;
        } else {
            float x0, x1;
            if (pos >= -w.pre && pos + 1 < w.loop_len) {
                // Both points inside [-pre, loop_len): no clamp or wrap, and set_wave checked that the
                // wave covers them (else the voice has ended), so read them directly.
                x0 = direct(w, pos);
                x1 = direct(w, pos + 1);
            } else {
                x0 = sample_at(w, pos);
                x1 = sample_at(w, pos + 1);
            }
            out[k] = x0 + (x1 - x0) * t;
        }
        const std::uint64_t f = static_cast<std::uint64_t>(w.frac) + inc_frac;
        w.frac = static_cast<std::uint32_t>(f);
        w.pos += inc_int + static_cast<std::int32_t>(f >> 32);
        if (w.loop_len == 0) {
            if (w.pos >= 0) w.ended = true;
        } else {
            while (w.pos >= w.loop_len) {
                const std::uint64_t g = static_cast<std::uint64_t>(w.frac) + w.loop_frac;
                w.frac = static_cast<std::uint32_t>(g);
                w.pos += static_cast<std::int32_t>(g >> 32) - w.loop_len;
            }
        }
    }
}

void svf50(float& band, float& low, float f, float df, float q, float* x, std::size_t n) {
    float b = band, l = low;
    for (std::size_t i = 0; i < n; ++i) {
        b += f * (x[i] - q * b - l);
        l += f * b;
        x[i] = l;
        f += df;
    }
    band = b;
    low = l;
}

namespace {
// The S-YXG2006LE's fetch: past the loop end wrapped (a one-shot holds its last sample), below the
// pre-loop clamped.
inline float fetch_le(const WalkLE& w, std::int64_t i) {
    if (i >= w.loop_len) i = w.one_shot ? w.loop_len - 1 : i - w.loop_len;
    if (i < -w.pre_loop) i = -w.pre_loop;
    return static_cast<float>(w.base[i]);
}
inline float hermite_le(float y0, float y1, float y2, float y3, float t) {
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}
} // namespace

void resample_le(WalkLE& w, std::int32_t inc, InterpLE interp, const float* lanczos, float* out, std::size_t n) {
    const std::int64_t last = w.loop_len - 1;
    for (std::size_t k = 0; k < n; ++k) {
        if (w.ended) {
            out[k] = 0.0f;
            continue;
        }
        float v;
        if (interp == InterpLE::Linear) {
            // 10-bit weights, w1 = 32·(frac >> 5).
            const float s0 = static_cast<float>(w.base[w.pos]);
            const float s1 = w.pos == last ? (w.one_shot ? s0 : static_cast<float>(w.base[0])) : static_cast<float>(w.base[w.pos + 1]);
            const float w1 = static_cast<float>(32 * (w.frac >> 5)) * (1.0f / 32768.0f);
            v = s0 + (s1 - s0) * w1;
        } else {
            const float fr = static_cast<float>(w.frac) * (1.0f / 32768.0f);
            if (interp == InterpLE::Hermite) {
                v = hermite_le(fetch_le(w, w.pos - 1), fetch_le(w, w.pos), fetch_le(w, w.pos + 1), fetch_le(w, w.pos + 2), fr);
            } else {
                const float* wt = lanczos + 6 * static_cast<std::size_t>(std::lround(fr * 256));
                v = 0.0f;
                for (int j = 0; j < 6; ++j) v += wt[j] * fetch_le(w, w.pos - 2 + j);
            }
        }
        out[k] = v;
        const std::int32_t acc = w.frac + inc;
        w.pos += acc >> kPhaseBitsLE;
        w.frac = acc & ((1 << kPhaseBitsLE) - 1);
        while (w.pos >= w.loop_len) {
            if (w.one_shot) {
                w.ended = true;
                break;
            }
            w.pos -= w.loop_len;
        }
    }
}

void svf_le_lanes(float* x, std::size_t stride, float* const* lp, float* const* bp, const float* f, const float* df,
                  const float* q, std::size_t lanes, std::size_t n) {
    for (std::size_t l = 0; l < lanes; ++l) {
        float lo = *lp[l], b = *bp[l], ff = f[l];
        float* xs = x + l * stride;
        for (std::size_t i = 0; i < n; ++i) {
            const float qq = std::min(q[l], std::max(3.0f - 2.0f * ff, 2.0f - ff));
            const float out = lo;
            b += ff * (xs[i] - qq * b - lo);
            lo += ff * b;
            xs[i] = out;
            ff += df[l];
        }
        *lp[l] = lo;
        *bp[l] = b;
    }
}

void peak_ramp(const float* x, float a, float d, float* peak, std::size_t n) {
    float hi = *peak, lo = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        const float y = a * x[i];
        if (y > hi) hi = y;
        else if (y < lo) lo = y;
        a += d;
    }
    *peak = std::max(hi, -lo);
}

void svf50_lanes(float* x, std::size_t stride, float* const* band, float* const* low, const float* f, const float* df,
                 const float* q, std::size_t lanes, std::size_t n) {
    for (std::size_t l = 0; l < lanes; ++l) svf50(*band[l], *low[l], f[l], df[l], q[l], x + l * stride, n);
}

void mix_ramps(const float* x, float* const* dst, const float* g, const float* dg, std::size_t buses, std::size_t n) {
    switch (buses) {
    case 0: return;
    case 1: mix_fixed<1>(x, dst, g, dg, n); return;
    case 2: mix_fixed<2>(x, dst, g, dg, n); return;
    case 3: mix_fixed<3>(x, dst, g, dg, n); return;
    case 4: mix_fixed<4>(x, dst, g, dg, n); return;
    case 5: mix_fixed<5>(x, dst, g, dg, n); return;
    case 6: mix_fixed<6>(x, dst, g, dg, n); return;
    default: mix_fixed<7>(x, dst, g, dg, n); return;
    }
}

} // namespace hvyxg::accel::ref
