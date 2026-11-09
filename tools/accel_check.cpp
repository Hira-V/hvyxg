// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// accel_check: each target kernel (accel/kernels.hpp) against its portable reference on random inputs
// (docs/ACCELERATION.md, "Accuracy"). Standalone (no assets, no doctest), so it runs where a target's
// build runs: under qemu (RVV at any VLEN), on the boards. Exit 0 when every kernel is within its bound.
//
//   accel_check [ROUNDS]
//
// resample50 and resample_le (their Hermite within 2e-6 of full scale; exact with HVYXG_ACCEL_EXACT), svf50_lanes and svf_le_lanes must be exact; peak_ramp within 1e-5; mix_ramps within 1e-5 of full scale (-100 dBFS): its ramps are closed form
// (g0 + i·dg), the reference's 128 additions of dg in a row, each rounded -- up to ~128 roundings apart.

#include "accel/kernels.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace hvyxg::accel;

namespace {

std::uint64_t g_rng = 0x9E3779B97F4A7C15ull;
std::uint32_t rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return static_cast<std::uint32_t>(g_rng >> 16);
}
std::uint32_t rnd(std::uint32_t n) { return n ? rnd() % n : 0; }

int g_failed = 0;

/// The S-YXG50's accelerated Hermite (kernels_rvv.cpp hermite_m4): its error bound, and the largest seen.
constexpr double kHermite50 = 2e-6;
double g_hermite50_worst = 0;
double g_hermite_le_worst = 0;              // the 2006LE's (the same form)

void check_resample(int rounds) {
    std::vector<std::uint8_t> wave;
    long samples = 0, blocks = 0;
    for (int r = 0; r < rounds; ++r) {
        const bool pcm8 = rnd(4) == 0, hermite = rnd(2) != 0, oneshot = rnd(4) == 0;
        const std::size_t bytes = pcm8 ? 1 : 2;
        const std::int32_t pre = static_cast<std::int32_t>(rnd(64)), loop_len = oneshot ? 0 : 1 + static_cast<std::int32_t>(rnd(r % 3 ? 4000 : 6));
        const std::size_t loop_start = (static_cast<std::size_t>(pre) + rnd(16)) * bytes;
        const std::size_t extra = rnd(3) ? 8 : rnd(3);   // sometimes the wave ends right at the loop
        wave.assign(loop_start + (static_cast<std::size_t>(loop_len) + extra + 4) * bytes, 0);
        for (auto& b : wave) b = static_cast<std::uint8_t>(rnd());
        Walk50 a;
        a.wave = wave.data();
        a.wave_size = wave.size() - (extra == 0 ? 4 * bytes : 0);
        a.loop_start = loop_start;
        a.pcm8 = pcm8;
        a.pre = pre;
        a.loop_len = loop_len;
        a.loop_frac = rnd();
        a.pos = -static_cast<std::int32_t>(rnd(static_cast<std::uint32_t>(pre) + 1));
        a.frac = rnd();
        if (!oneshot && rnd(2)) a.pos = static_cast<std::int32_t>(rnd(static_cast<std::uint32_t>(loop_len)));
        Walk50 b = a;
        for (int blk = 0; blk < 8; ++blk) {
            // steps from very slow to ~16 samples a sample (the DLL's range: up to 2^22 << 17)
            const std::uint64_t inc = (static_cast<std::uint64_t>(rnd(1u << 22)) << 17) >> rnd(12);
            const auto ii = static_cast<std::int32_t>(inc >> 32);
            const auto fi = static_cast<std::uint32_t>(inc);
            const std::size_t n = 1 + rnd(128);
            float ya[128], yb[128];
            ref::resample50(a, ii, fi, hermite, ya, n);
#if defined(HVYXG_ACCEL_RVV)
            rvv::resample50(b, ii, fi, hermite, yb, n);
#else
            ref::resample50(b, ii, fi, hermite, yb, n);
#endif
            ++blocks;
            samples += static_cast<long>(n);
            // Hermite in the accelerated build: the fused form, within kHermite50 of full scale (exact with
            // HVYXG_ACCEL_EXACT); linear, and the walk, always exact.
            bool same = std::memcmp(ya, yb, n * sizeof(float)) == 0;
#if defined(HVYXG_ACCEL_RVV) && !defined(HVYXG_ACCEL_EXACT)
            if (!same && hermite) {
                same = true;
                for (std::size_t i = 0; i < n; ++i) {
                    const double e = std::fabs(static_cast<double>(ya[i]) - static_cast<double>(yb[i]));
                    g_hermite50_worst = e > g_hermite50_worst ? e : g_hermite50_worst;
                    if (e > kHermite50) same = false;
                }
            }
#endif
            if (!same || a.pos != b.pos || a.frac != b.frac || a.ended != b.ended) {
                if (g_failed++ < 5) {
                    std::size_t i = 0;
                    while (i < n && std::memcmp(&ya[i], &yb[i], 4) == 0) ++i;
                    std::printf("resample50 DIFFERS: round %d block %d (pcm8 %d hermite %d loop_len %d pre %d inc %#llx n %zu): "
                                "sample %zu ref %.9g got %.9g; pos %d/%d frac %u/%u ended %d/%d\n",
                                r, blk, pcm8, hermite, loop_len, pre, static_cast<unsigned long long>(inc), n, i,
                                i < n ? ya[i] : 0.0, i < n ? yb[i] : 0.0, a.pos, b.pos, a.frac, b.frac, a.ended, b.ended);
                }
                break;
            }
        }
    }
    std::printf("resample50: %ld blocks, %ld samples; Hermite's largest error %.3g of full scale\n", blocks, samples, g_hermite50_worst);
}

void check_mix(int rounds) {
    double worst = 0;
    for (int r = 0; r < rounds; ++r) {
        const std::size_t buses = 1 + rnd(kMaxBuses), n = 1 + rnd(128);
        float x[128], da[kMaxBuses][128], db[kMaxBuses][128], g[kMaxBuses], dg[kMaxBuses];
        float* pa[kMaxBuses];
        float* pb[kMaxBuses];
        for (std::size_t i = 0; i < n; ++i) x[i] = static_cast<float>(static_cast<std::int32_t>(rnd()) >> 8) * (1.0f / 8388608.0f);
        for (std::size_t b = 0; b < buses; ++b) {
            g[b] = static_cast<float>(rnd(1 << 20)) * (1.0f / 1048576.0f);
            dg[b] = (static_cast<float>(rnd(1 << 20)) * (1.0f / 1048576.0f) - g[b]) * (1.0f / 128.0f);
            for (std::size_t i = 0; i < n; ++i) da[b][i] = db[b][i] = static_cast<float>(static_cast<std::int32_t>(rnd()) >> 8) * (1.0f / 8388608.0f);
            pa[b] = da[b];
            pb[b] = db[b];
        }
        ref::mix_ramps(x, pa, g, dg, buses, n);
#if defined(HVYXG_ACCEL_RVV)
        rvv::mix_ramps(x, pb, g, dg, buses, n);
#else
        ref::mix_ramps(x, pb, g, dg, buses, n);
#endif
        for (std::size_t b = 0; b < buses; ++b)
            for (std::size_t i = 0; i < n; ++i) worst = std::fmax(worst, std::fabs(static_cast<double>(da[b][i]) - db[b][i]));
    }
    std::printf("mix_ramps: largest error %.3g of full scale (%.1f dBFS)\n", worst, worst > 0 ? 20 * std::log10(worst) : -999.0);
    if (worst > 1e-5) {
        std::printf("mix_ramps OUT OF BOUND (1e-5)\n");
        ++g_failed;
    }
}

void check_svf_lanes(int rounds) {
    long voices = 0;
    for (int r = 0; r < rounds / 8; ++r) {
        const std::size_t lanes = 1 + rnd(kBatch), n = 1 + rnd(128), stride = 128;
        static float xa[kBatch * 128], xb[kBatch * 128];
        float ba[kBatch], la[kBatch], bb[kBatch], lb[kBatch], f[kBatch], df[kBatch], q[kBatch];
        float* pba[kBatch];
        float* pla[kBatch];
        float* pbb[kBatch];
        float* plb[kBatch];
        for (std::size_t l = 0; l < lanes; ++l) {
            for (std::size_t i = 0; i < n; ++i) xa[l * stride + i] = xb[l * stride + i] = static_cast<float>(static_cast<std::int32_t>(rnd()) >> 8) * (1.0f / 8388608.0f);
            ba[l] = bb[l] = static_cast<float>(static_cast<std::int32_t>(rnd()) >> 10) * (1.0f / 8388608.0f);
            la[l] = lb[l] = static_cast<float>(static_cast<std::int32_t>(rnd()) >> 10) * (1.0f / 8388608.0f);
            f[l] = static_cast<float>(rnd(1 << 20)) * (1.0f / 1048576.0f);
            df[l] = (static_cast<float>(rnd(1 << 20)) * (1.0f / 1048576.0f) - f[l]) * (1.0f / 128.0f);
            q[l] = 0.1f + static_cast<float>(rnd(1 << 20)) * (1.9f / 1048576.0f);
            pba[l] = &ba[l], pla[l] = &la[l], pbb[l] = &bb[l], plb[l] = &lb[l];
        }
        ref::svf50_lanes(xa, stride, pba, pla, f, df, q, lanes, n);
#if defined(HVYXG_ACCEL_RVV)
        rvv::svf50_lanes(xb, stride, pbb, plb, f, df, q, lanes, n);
#else
        ref::svf50_lanes(xb, stride, pbb, plb, f, df, q, lanes, n);
#endif
        voices += static_cast<long>(lanes);
        bool same = !std::memcmp(ba, bb, lanes * 4) && !std::memcmp(la, lb, lanes * 4);
        for (std::size_t l = 0; l < lanes && same; ++l) same = !std::memcmp(xa + l * stride, xb + l * stride, n * 4);
        if (!same && g_failed++ < 5) std::printf("svf50_lanes DIFFERS: round %d (lanes %zu, n %zu)\n", r, lanes, n);
    }
    std::printf("svf50_lanes: %ld voice blocks\n", voices);
}

#if defined(HVYXG_ACCEL_RVV)
#define TARGET(f) rvv::f
#else
#define TARGET(f) ref::f
#endif

void check_resample_le(int rounds) {
    std::vector<std::int16_t> wave;
    static float lanczos[257 * 6];
    for (std::size_t i = 0; i < 257 * 6; ++i) lanczos[i] = static_cast<float>(static_cast<std::int32_t>(rnd()) >> 8) * (1.0f / 8388608.0f);
    long samples = 0;
    for (int r = 0; r < rounds; ++r) {
        const auto interp = static_cast<InterpLE>(rnd(3));
        const bool oneshot = rnd(4) == 0;
        const std::int64_t pre = rnd(64), len = 2 + rnd(r % 3 ? 4000 : 6);
        wave.assign(static_cast<std::size_t>(pre + len + 8), 0);
        for (auto& v : wave) v = static_cast<std::int16_t>(rnd());
        WalkLE a;
        a.base = wave.data() + pre;
        a.pre_loop = pre;
        a.loop_len = len;
        a.one_shot = oneshot;
        a.pos = rnd(2) ? -static_cast<std::int64_t>(rnd(static_cast<std::uint32_t>(pre) + 1)) : static_cast<std::int64_t>(rnd(static_cast<std::uint32_t>(len)));
        a.frac = static_cast<std::int32_t>(rnd(1u << kPhaseBitsLE));
        WalkLE b = a;
        for (int blk = 0; blk < 8; ++blk) {
            const auto inc = static_cast<std::int32_t>(rnd(1u << 22) >> rnd(10));
            const std::size_t n = 1 + rnd(64);
            float ya[64], yb[64];
            ref::resample_le(a, inc, interp, lanczos, ya, n);
            TARGET(resample_le)(b, inc, interp, lanczos, yb, n);
            samples += static_cast<long>(n);
            // Hermite in the accelerated build: the fused form, within kHermite50 of full scale (32768 here);
            // linear, Lanczos and the walk exact.
            bool same = std::memcmp(ya, yb, n * 4) == 0;
#if defined(HVYXG_ACCEL_RVV) && !defined(HVYXG_ACCEL_EXACT)
            if (!same && interp == InterpLE::Hermite) {
                same = true;
                for (std::size_t i = 0; i < n; ++i) {
                    const double e = std::fabs(static_cast<double>(ya[i]) - static_cast<double>(yb[i])) / 32768.0;
                    g_hermite_le_worst = e > g_hermite_le_worst ? e : g_hermite_le_worst;
                    if (e > kHermite50) same = false;
                }
            }
#endif
            if (!same || a.pos != b.pos || a.frac != b.frac || a.ended != b.ended) {
                if (g_failed++ < 5)
                    std::printf("resample_le DIFFERS: round %d block %d (interp %d one-shot %d len %lld pre %lld inc %d n %zu)\n", r, blk,
                                static_cast<int>(interp), oneshot, static_cast<long long>(len), static_cast<long long>(pre), inc, n);
                break;
            }
        }
    }
    std::printf("resample_le: %ld samples; Hermite's largest error %.3g of full scale\n", samples, g_hermite_le_worst);
}

void check_svf_le_lanes(int rounds) {
    long voices = 0;
    for (int r = 0; r < rounds / 8; ++r) {
        const std::size_t lanes = 1 + rnd(kBatch), n = 1 + rnd(64), stride = 64;
        static float xa[kBatch * 64], xb[kBatch * 64];
        float la[kBatch], ba[kBatch], lb[kBatch], bb[kBatch], f[kBatch], df[kBatch], q[kBatch];
        float* pla[kBatch];
        float* pba[kBatch];
        float* plb[kBatch];
        float* pbb[kBatch];
        for (std::size_t l = 0; l < lanes; ++l) {
            for (std::size_t i = 0; i < n; ++i) xa[l * stride + i] = xb[l * stride + i] = static_cast<float>(static_cast<std::int16_t>(rnd()));
            la[l] = lb[l] = static_cast<float>(static_cast<std::int16_t>(rnd()));
            ba[l] = bb[l] = static_cast<float>(static_cast<std::int16_t>(rnd()));
            f[l] = static_cast<float>(rnd(1 << 20)) * (1.0f / 1048576.0f);
            df[l] = (static_cast<float>(rnd(1 << 20)) * (1.0f / 1048576.0f) - f[l]) * (1.0f / 64.0f);
            q[l] = 0.1f + static_cast<float>(rnd(1 << 20)) * (2.9f / 1048576.0f);
            pla[l] = &la[l], pba[l] = &ba[l], plb[l] = &lb[l], pbb[l] = &bb[l];
        }
        ref::svf_le_lanes(xa, stride, pla, pba, f, df, q, lanes, n);
        TARGET(svf_le_lanes)(xb, stride, plb, pbb, f, df, q, lanes, n);
        voices += static_cast<long>(lanes);
        bool same = !std::memcmp(la, lb, lanes * 4) && !std::memcmp(ba, bb, lanes * 4);
        for (std::size_t l = 0; l < lanes && same; ++l) same = !std::memcmp(xa + l * stride, xb + l * stride, n * 4);
        if (!same && g_failed++ < 5) std::printf("svf_le_lanes DIFFERS: round %d (lanes %zu, n %zu)\n", r, lanes, n);
    }
    std::printf("svf_le_lanes: %ld voice blocks\n", voices);
}

void check_peak(int rounds) {
    double worst = 0;
    for (int r = 0; r < rounds; ++r) {
        const std::size_t n = 1 + rnd(64);
        float x[64];
        for (std::size_t i = 0; i < n; ++i) x[i] = static_cast<float>(static_cast<std::int16_t>(rnd()));
        const float a = static_cast<float>(rnd(1 << 20)) * (1.0f / 1048576.0f) / 32768.0f;
        const float d = (static_cast<float>(rnd(1 << 20)) * (1.0f / 1048576.0f) / 32768.0f - a) / 64.0f;
        const float p0 = static_cast<float>(rnd(1 << 16)) * (1.0f / 65536.0f) * 0.5f;
        float pa = p0, pb = p0;
        ref::peak_ramp(x, a, d, &pa, n);
        TARGET(peak_ramp)(x, a, d, &pb, n);
        worst = std::fmax(worst, std::fabs(static_cast<double>(pa) - pb));
    }
    std::printf("peak_ramp: largest error %.3g of full scale\n", worst);
    if (worst > 1e-5) {
        std::printf("peak_ramp OUT OF BOUND (1e-5)\n");
        ++g_failed;
    }
}

} // namespace

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 20000;
#if defined(HVYXG_ACCEL_RVV)
    std::printf("accel_check: the RVV kernels against the reference, %d rounds\n", rounds);
#else
    std::printf("accel_check: no target kernels in this build (the reference against itself)\n");
#endif
    check_resample(rounds);
    check_mix(rounds);
    check_svf_lanes(rounds);
    check_resample_le(rounds);
    check_svf_le_lanes(rounds);
    check_peak(rounds);
    std::printf(g_failed ? "accel_check: FAILED\n" : "accel_check: all within bounds\n");
    return g_failed ? 1 : 0;
}
