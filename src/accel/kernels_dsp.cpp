// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// The A25's kernels (Andes; docs/ACCELERATION.md, "The A25"), f32 on its scalar FPU. The A25 is in
// order and issues one instruction a cycle, so these are the references' arithmetic reshaped for it:
// the resamplers keep the walk in registers over runs that need no edge checks (the references store
// the walk to memory every sample, as `out` might alias it), and read the samples as integer-valued
// floats scaled once; the filters run four voices at a time, so that four dependent chains overlap;
// interpolation and mixing use fused multiply-adds -- the accelerated forms, within the bounds that
// docs/ACCELERATION.md gives, as the RVV kernels' (HVYXG_ACCEL_EXACT: the references' operations, bit
// for bit). Built only with HVYXG_ACCEL_DSP.

#if defined(HVYXG_ACCEL_DSP)

#include "accel/kernels.hpp"

#include <cstddef>
#include <cstdint>

namespace hvyxg::accel::dsp {

namespace {

#if defined(HVYXG_ACCEL_EXACT)
inline float lerp(float x0, float x1, float t) { return x0 + (x1 - x0) * t; }
inline float hermite(float xm1, float x0, float x1, float x2, float t) {
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}
#else
// The accelerated forms, as kernels_rvv.cpp's lerp_m4 and hermite_m4.
inline float lerp(float x0, float x1, float t) { return __builtin_fmaf(x1 - x0, t, x0); }
inline float hermite(float xm1, float x0, float x1, float x2, float t) {
    const float c1 = (x1 - xm1) * 0.5f;
    const float d01 = x0 - x1;
    const float c3 = __builtin_fmaf(1.5f, d01, (x2 - xm1) * 0.5f);
    const float c2 = -(d01 + c1) - c3;   // (x1 - x0) - c1 - c3
    float y = __builtin_fmaf(c3, t, c2);
    y = __builtin_fmaf(y, t, c1);
    return __builtin_fmaf(y, t, x0);
}
#endif

// How many samples from (pos, frac), stepping inc (32.32), keep the position below `limit`: the k ≥ 0
// with pos + ⌊(frac + k·inc) / 2³²⌋ < limit, at most n.
std::size_t run_below(std::int32_t pos, std::uint32_t frac, std::uint64_t inc, std::int64_t limit, std::size_t n) {
    if (pos >= limit) return 0;
    if (!inc) return n;
    const std::uint64_t room = (static_cast<std::uint64_t>(limit - pos) << 32) - frac;
    const std::uint64_t k = (room + inc - 1) / inc;
    return k < n ? static_cast<std::size_t>(k) : n;
}

// The samples as integer-valued floats (the reference's -0x80 / -0x8000, before its scale).
inline float s8(const std::uint8_t* p) { return static_cast<float>(static_cast<std::int8_t>(*p ^ 0x80)); }
inline float s16a(const std::uint8_t* p) {   // 16-bit aligned
    return static_cast<float>(static_cast<std::int16_t>(*reinterpret_cast<const std::uint16_t*>(p) ^ 0x8000));
}
inline float s16u(const std::uint8_t* p) {   // any address
    return static_cast<float>(static_cast<std::int16_t>((p[0] | p[1] << 8) ^ 0x8000));
}

// m samples of the S-YXG50's walk, all with their points inside [-pre, U): no edge checks. The walk
// is read, not advanced (the caller does it once for the run).
template <int Bytes, bool Aligned, bool Hermite>
__attribute__((noinline)) void run50(const Walk50& w, std::int32_t inc_int, std::uint32_t inc_frac, float* out, std::size_t m) {
    const std::uint8_t* base = w.wave + w.loop_start;   // sample 0
    const float scale = Bytes == 1 ? 1.0f / 128.0f : 1.0f / 32768.0f;
    const auto at = [&](std::int32_t i) {
        const std::uint8_t* p = base + Bytes * static_cast<std::ptrdiff_t>(i);
        return Bytes == 1 ? s8(p) : Aligned ? s16a(p) : s16u(p);
    };
    std::int32_t pos = w.pos;
    std::uint32_t frac = w.frac;
    // unrolled: four samples' chains (load, convert, interpolate, scale, store) side by side, for the
    // in-order core to overlap
#pragma GCC unroll 4
    for (std::size_t k = 0; k < m; ++k) {
        const float t = static_cast<float>(frac) * (1.0f / 4294967296.0f);
        const float y = Hermite ? hermite(at(pos - 1), at(pos), at(pos + 1), at(pos + 2), t) : lerp(at(pos), at(pos + 1), t);
        out[k] = y * scale;   // a power of two: the reference's rounding, bit for bit (kernels_rvv.cpp)
        const std::uint32_t f = frac + inc_frac;
        pos += inc_int + (f < frac ? 1 : 0);
        frac = f;
    }
}

template <bool Hermite>
void segment50(const Walk50& w, std::int32_t inc_int, std::uint32_t inc_frac, float* out, std::size_t m) {
    if (w.pcm8) return run50<1, true, Hermite>(w, inc_int, inc_frac, out, m);
    if (!(reinterpret_cast<std::uintptr_t>(w.wave + w.loop_start) & 1)) return run50<2, true, Hermite>(w, inc_int, inc_frac, out, m);
    run50<2, false, Hermite>(w, inc_int, inc_frac, out, m);
}

// The 2006LE's runs: m samples whose points all lie inside the direct range (no wrap, no clamp). The
// walk's fraction in 15 bits, accumulated in 32 (the caller bounds the step).
template <InterpLE Interp>
__attribute__((noinline)) void run_le(const WalkLE& w, std::int32_t inc, const float* lanczos, float* out, std::size_t m) {
    const std::int16_t* base = w.base + w.pos;
    std::uint32_t acc = static_cast<std::uint32_t>(w.frac);
    const auto uinc = static_cast<std::uint32_t>(inc);
#pragma GCC unroll 4
    for (std::size_t k = 0; k < m; ++k, acc += uinc) {
        const std::int16_t* p = base + (acc >> kPhaseBitsLE);
        const std::uint32_t fr = acc & ((1u << kPhaseBitsLE) - 1);
        float v;
        if (Interp == InterpLE::Linear) {
            const float w1 = static_cast<float>(fr >> 5 << 5) * (1.0f / 32768.0f);   // 10-bit weights
            v = lerp(static_cast<float>(p[0]), static_cast<float>(p[1]), w1);
        } else if (Interp == InterpLE::Hermite) {
            const float t = static_cast<float>(fr) * (1.0f / 32768.0f);
            v = hermite(static_cast<float>(p[-1]), static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]), t);
        } else {
            const float* wt = lanczos + 6 * ((fr + 64) >> 7);   // lround(fr / 128)
#if defined(HVYXG_ACCEL_EXACT)
            v = 0.0f;   // (as the reference: 0 + a product, so a zero comes out +0)
            for (int j = 0; j < 6; ++j) v += wt[j] * static_cast<float>(p[j - 2]);
#else
            v = wt[0] * static_cast<float>(p[-2]);
            for (int j = 1; j < 6; ++j) v = __builtin_fmaf(wt[j], static_cast<float>(p[j - 2]), v);
#endif
        }
        out[k] = v;
    }
}

// A recursive filter for `lanes` voices, four at a time: each voice's operations exactly as the
// reference's (so the result is bit for bit), the four chains interleaved for the in-order core.
// Step(x, s1, s2, f, q) -> y.
template <class Step>
void lanes4(float* x, std::size_t stride, float* const* s1, float* const* s2, const float* f, const float* df,
            const float* q, std::size_t lanes, std::size_t n, Step step) {
    std::size_t l = 0;
    for (; l + 4 <= lanes; l += 4) {
        float a0 = *s1[l], a1 = *s1[l + 1], a2 = *s1[l + 2], a3 = *s1[l + 3];
        float b0 = *s2[l], b1 = *s2[l + 1], b2 = *s2[l + 2], b3 = *s2[l + 3];
        float f0 = f[l], f1 = f[l + 1], f2 = f[l + 2], f3 = f[l + 3];
        const float d0 = df[l], d1 = df[l + 1], d2 = df[l + 2], d3 = df[l + 3];
        const float q0 = q[l], q1 = q[l + 1], q2 = q[l + 2], q3 = q[l + 3];
        float* x0 = x + l * stride;
        float* x1 = x0 + stride;
        float* x2 = x1 + stride;
        float* x3 = x2 + stride;
        for (std::size_t i = 0; i < n; ++i) {
            x0[i] = step(x0[i], a0, b0, f0, q0);
            x1[i] = step(x1[i], a1, b1, f1, q1);
            x2[i] = step(x2[i], a2, b2, f2, q2);
            x3[i] = step(x3[i], a3, b3, f3, q3);
            f0 += d0, f1 += d1, f2 += d2, f3 += d3;
        }
        *s1[l] = a0, *s1[l + 1] = a1, *s1[l + 2] = a2, *s1[l + 3] = a3;
        *s2[l] = b0, *s2[l + 1] = b1, *s2[l + 2] = b2, *s2[l + 3] = b3;
    }
    for (; l < lanes; ++l) {
        float a = *s1[l], b = *s2[l], ff = f[l];
        float* xs = x + l * stride;
        for (std::size_t i = 0; i < n; ++i) {
            xs[i] = step(xs[i], a, b, ff, q[l]);
            ff += df[l];
        }
        *s1[l] = a, *s2[l] = b;
    }
}

} // namespace

void resample50(Walk50& w, std::int32_t inc_int, std::uint32_t inc_frac, bool hermite, float* out, std::size_t n) {
    const std::uint64_t inc = static_cast<std::uint64_t>(static_cast<std::uint32_t>(inc_int)) << 32 | inc_frac;
    const std::int32_t lo = hermite ? 1 : 0, hi = hermite ? 2 : 1;   // the points around a position
    const std::size_t bytes = w.pcm8 ? 1 : 2;
    const std::int64_t avail = w.wave_size > w.loop_start ? static_cast<std::int64_t>((w.wave_size - w.loop_start) / bytes) : 0;
    std::size_t k = 0;
    while (k < n) {
        if (w.ended) {
            for (; k < n; ++k) out[k] = 0.0f;
            return;
        }
        // the run whose points all lie inside [-pre, U), as kernels_rvv.cpp's resample50
        std::size_t m = 0;
        if (w.pos - lo >= -w.pre) {
            if (w.loop_len > 0) {
                m = run_below(w.pos, w.frac, inc, static_cast<std::int64_t>(w.loop_len) - hi, n - k);
            } else {
                const std::int64_t cap = avail - hi < 0 ? avail - hi : 0;
                m = run_below(w.pos, w.frac, inc, cap, n - k);
            }
        }
        if (m == 0) {   // a position at an edge: the reference, one sample
            ref::resample50(w, inc_int, inc_frac, hermite, out + k, 1);
            ++k;
            continue;
        }
        if (hermite) segment50<true>(w, inc_int, inc_frac, out + k, m);
        else segment50<false>(w, inc_int, inc_frac, out + k, m);
        const std::uint64_t acc = static_cast<std::uint64_t>(w.frac) + static_cast<std::uint64_t>(m) * inc;
        w.frac = static_cast<std::uint32_t>(acc);
        w.pos += static_cast<std::int32_t>(acc >> 32);
        if (w.loop_len == 0) {
            if (w.pos >= 0) w.ended = true;
        } else {
            while (w.pos >= w.loop_len) {
                const std::uint64_t g = static_cast<std::uint64_t>(w.frac) + w.loop_frac;
                w.frac = static_cast<std::uint32_t>(g);
                w.pos += static_cast<std::int32_t>(g >> 32) - w.loop_len;
            }
        }
        k += m;
    }
}

void resample_le(WalkLE& w, std::int32_t inc, InterpLE interp, const float* lanczos, float* out, std::size_t n) {
    // A step whose fractions could pass 32 bits over a block stays the reference's.
    if (inc < 0 || inc > (1 << 24) || n > 128) {
        ref::resample_le(w, inc, interp, lanczos, out, n);
        return;
    }
    const bool hermite = interp == InterpLE::Hermite;
    const std::int64_t lo = interp == InterpLE::Lanczos ? 2 : hermite ? 1 : 0, hi = interp == InterpLE::Lanczos ? 3 : hermite ? 2 : 1;
    std::size_t k = 0;
    while (k < n) {
        if (w.ended) {
            for (; k < n; ++k) out[k] = 0.0f;
            return;
        }
        // the run whose points all lie inside [-pre_loop, loop_len), as kernels_rvv.cpp's resample_le
        std::size_t m = 0;
        if (w.pos - lo >= -w.pre_loop && w.pos < w.loop_len - hi) {
            if (inc > 0) {
                const std::uint64_t room = (static_cast<std::uint64_t>(w.loop_len - hi - w.pos) << kPhaseBitsLE) - static_cast<std::uint64_t>(w.frac);
                const std::uint64_t r = (room + static_cast<std::uint64_t>(inc) - 1) / static_cast<std::uint64_t>(inc);
                m = r < n - k ? static_cast<std::size_t>(r) : n - k;
            } else {
                m = n - k;
            }
        }
        if (m == 0) {
            ref::resample_le(w, inc, interp, lanczos, out + k, 1);
            ++k;
            continue;
        }
        if (interp == InterpLE::Lanczos) run_le<InterpLE::Lanczos>(w, inc, lanczos, out + k, m);
        else if (hermite) run_le<InterpLE::Hermite>(w, inc, lanczos, out + k, m);
        else run_le<InterpLE::Linear>(w, inc, lanczos, out + k, m);
        const std::uint64_t acc = static_cast<std::uint64_t>(w.frac) + static_cast<std::uint64_t>(m) * static_cast<std::uint64_t>(inc);
        w.pos += static_cast<std::int64_t>(acc >> kPhaseBitsLE);
        w.frac = static_cast<std::int32_t>(acc & ((1u << kPhaseBitsLE) - 1));
        while (w.pos >= w.loop_len) {
            if (w.one_shot) {
                w.ended = true;
                break;
            }
            w.pos -= w.loop_len;
        }
        k += m;
    }
}

void svf50_lanes(float* x, std::size_t stride, float* const* band, float* const* low, const float* f, const float* df,
                 const float* q, std::size_t lanes, std::size_t n) {
    // b += f (x - q b - l); l += f b; y = l -- the reference's operations in its order (svf50)
    lanes4(x, stride, band, low, f, df, q, lanes, n, [](float xi, float& b, float& l, float ff, float qq) {
        b += ff * (xi - qq * b - l);
        l += ff * b;
        return l;
    });
}

void svf_le_lanes(float* x, std::size_t stride, float* const* lp, float* const* bp, const float* f, const float* df,
                  const float* q, std::size_t lanes, std::size_t n) {
    // qq = min(q, max(3 - 2f, 2 - f)); y = lp; bp += f (x - qq bp - lp); lp += f bp -- the reference's
    lanes4(x, stride, lp, bp, f, df, q, lanes, n, [](float xi, float& lo, float& b, float ff, float qv) {
        const float m = 3.0f - 2.0f * ff, m2 = 2.0f - ff;
        const float lim = m > m2 ? m : m2;
        const float qq = qv < lim ? qv : lim;
        const float out = lo;
        b += ff * (xi - qq * b - lo);
        lo += ff * b;
        return out;
    });
}

void mix_ramps(const float* x, float* const* dst, const float* g, const float* dg, std::size_t buses, std::size_t n) {
    if (buses > kMaxBuses) buses = kMaxBuses;
#if defined(HVYXG_ACCEL_EXACT)
    // the reference's ramps (g stepping by dg, sample by sample) and products, bus by bus
    for (std::size_t b = 0; b < buses; ++b) {
        float gg = g[b];
        float* d = dst[b];
        for (std::size_t i = 0; i < n; ++i) {
            d[i] += x[i] * gg;
            gg += dg[b];
        }
    }
#else
    // The ramps in closed form, g + i·dg (as rvv::mix_ramps; within float rounding of the reference's
    // running sum), so that no sample waits for the one before it; the product added fused. Four
    // samples a step for the in-order core.
    for (std::size_t b = 0; b < buses; ++b) {
        const float g0 = g[b], e = dg[b];
        float* d = dst[b];
        std::size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            const float fi = static_cast<float>(i);
            const float a0 = __builtin_fmaf(fi, e, g0), a1 = __builtin_fmaf(fi + 1.0f, e, g0);
            const float a2 = __builtin_fmaf(fi + 2.0f, e, g0), a3 = __builtin_fmaf(fi + 3.0f, e, g0);
            const float y0 = __builtin_fmaf(x[i], a0, d[i]), y1 = __builtin_fmaf(x[i + 1], a1, d[i + 1]);
            const float y2 = __builtin_fmaf(x[i + 2], a2, d[i + 2]), y3 = __builtin_fmaf(x[i + 3], a3, d[i + 3]);
            d[i] = y0, d[i + 1] = y1, d[i + 2] = y2, d[i + 3] = y3;
        }
        for (; i < n; ++i) d[i] = __builtin_fmaf(x[i], __builtin_fmaf(static_cast<float>(i), e, g0), d[i]);
    }
#endif
}

void peak_ramp(const float* x, float a, float d, float* peak, std::size_t n) {
    // max(*peak, max |a · x[i]|), a stepping by d -- the reference's max(hi, -lo) (rvv::peak_ramp)
    float m = *peak;
    for (std::size_t i = 0; i < n; ++i) {
        const float y = __builtin_fabsf(a * x[i]);
        m = y > m ? y : m;
        a += d;
    }
    *peak = m;
}



} // namespace hvyxg::accel::dsp

#endif
