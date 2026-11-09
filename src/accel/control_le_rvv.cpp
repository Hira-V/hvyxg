// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// The S-YXG2006LE voice's control across lanes in RVV (control_le.hpp): RVV 1.0 (the X280) and
// XTheadVector (the C920) from one source. The voices' state by indexed loads and stores, in place. Exact against syxg2006le::DspVoice::control for the lanes
// it keeps (ok): the integer steps as the reference's, the float operations unfused in its order
// (-ffp-contract=off), its x87 products (float × float in double, truncated) as widening multiplies --
// exact, since a float product always fits a double -- and its power-of-two scalings (ldexp) as a
// multiply by an exact power of two.

#if defined(HVYXG_ACCEL_RVV) && (defined(__riscv_vector) || defined(__riscv_xtheadvector))

#include "accel/control_le.hpp"

#include <riscv_vector.h>

#include <cstddef>
#include <cstdint>

namespace hvyxg::accel::rvv {

namespace {

using vi = vint32m1_t;
using vu = vuint32m1_t;
using vf = vfloat32m1_t;
using vb = vbool32_t;

constexpr std::int32_t kSilent = 0x100000;
constexpr float kBusScale = -1.0f / 8192.0f;

inline vi ld(const std::int32_t* p, std::size_t vl) { return __riscv_vle32_v_i32m1(p, vl); }
inline vf ldf(const float* p, std::size_t vl) { return __riscv_vle32_v_f32m1(p, vl); }
inline void st(std::int32_t* p, vi v, std::size_t vl) { __riscv_vse32_v_i32m1(p, v, vl); }
inline void stf(float* p, vf v, std::size_t vl) { __riscv_vse32_v_f32m1(p, v, vl); }
inline vi sel(vb m, vi yes, vi no, std::size_t vl) { return __riscv_vmerge_vvm_i32m1(no, yes, m, vl); }
inline vf self(vb m, vf yes, vf no, std::size_t vl) { return __riscv_vmerge_vvm_f32m1(no, yes, m, vl); }
inline vi splat(std::int32_t x, std::size_t vl) { return __riscv_vmv_v_x_i32m1(x, vl); }
inline vf splatf(float x, std::size_t vl) { return __riscv_vfmv_v_f_f32m1(x, vl); }
inline vb eqx(vi a, std::int32_t x, std::size_t vl) { return __riscv_vmseq_vx_i32m1_b32(a, x, vl); }

// Truncating conversions (the reference's static_cast<int32_t> / __ftol). RVV 0.7.1 has no .rtz forms:
// there the rounding mode is set to towards-zero around the conversion.
#if defined(__riscv_xtheadvector)
struct Rtz {
    unsigned long old;
    Rtz() { __asm__ volatile("fsrmi %0, 1" : "=r"(old)); }
    ~Rtz() { __asm__ volatile("fsrm %0" ::"r"(old)); }
};
inline vi trunc_f(vf x, std::size_t vl) { Rtz r; return __riscv_vfcvt_x_f_v_i32m1(x, vl); }
inline vi trunc_d(vfloat64m2_t x, std::size_t vl) { Rtz r; return __riscv_vfncvt_x_f_w_i32m1(x, vl); }
#else
inline vi trunc_f(vf x, std::size_t vl) { return __riscv_vfcvt_rtz_x_f_v_i32m1(x, vl); }
inline vi trunc_d(vfloat64m2_t x, std::size_t vl) { return __riscv_vfncvt_rtz_x_f_w_i32m1(x, vl); }
#endif
/// ftol(double(a) · double(b)) for floats a, b: the exact product, truncated.
inline vi ftol_mul(vf a, vf b, std::size_t vl) { return trunc_d(__riscv_vfwmul_vv_f64m2(a, b, vl), vl); }
/// 2^e as a float, for e in the normal range.
inline vf pow2(vi e, std::size_t vl) {
    return __riscv_vreinterpret_v_i32m1_f32m1(__riscv_vsll_vx_i32m1(__riscv_vadd_vx_i32m1(e, 127, vl), 23, vl));
}

/// The reference's step (dsp_voice.cpp): p toward target by inc; done once there (landing exactly on the
/// target reports not done; already there reports done without moving).
inline vi step(vi p, vi target, vi inc, vb& done, std::size_t vl) {
    const vb eq = __riscv_vmseq_vv_i32m1_b32(p, target, vl);
    const vb lt = __riscv_vmslt_vv_i32m1_b32(p, target, vl);
    const vi up = __riscv_vadd_vv_i32m1(p, inc, vl);
    const vi dn = __riscv_vsub_vv_i32m1(p, inc, vl);
    const vb over = __riscv_vmand_mm_b32(lt, __riscv_vmslt_vv_i32m1_b32(target, up, vl), vl);
    const vb gt = __riscv_vmnor_mm_b32(lt, eq, vl);
    const vb under = __riscv_vmand_mm_b32(gt, __riscv_vmslt_vv_i32m1_b32(dn, target, vl), vl);
    vi r = sel(lt, up, dn, vl);
    r = sel(__riscv_vmor_mm_b32(over, under, vl), target, r, vl);
    r = sel(eq, p, r, vl);
    done = __riscv_vmor_mm_b32(eq, __riscv_vmor_mm_b32(over, under, vl), vl);
    return r;
}
/// The reference's step_up: upward only; done at or past the target.
inline vi step_up(vi p, vi target, std::int32_t inc, vb& done, std::size_t vl) {
    const vb at = __riscv_vmsge_vv_i32m1_b32(p, target, vl);
    const vi up = __riscv_vadd_vx_i32m1(p, inc, vl);
    const vb over = __riscv_vmslt_vv_i32m1_b32(target, up, vl);
    done = __riscv_vmor_mm_b32(at, over, vl);
    return sel(at, p, sel(over, target, up, vl), vl);
}

/// eg_gain: 1 below 0, 0 from 0xFFFFF, else mant / (131071 · 2^octave).
inline vf eg_gain(vi x, std::size_t vl) {
    const vi mant = __riscv_vor_vx_i32m1(__riscv_vand_vx_i32m1(__riscv_vnot_v_i32m1(x, vl), 0xFFFF, vl), 0x10000, vl);
    const vi oct = __riscv_vand_vx_i32m1(__riscv_vsra_vx_i32m1(x, 16, vl), 15, vl);   // in range where used
    const vf den = __riscv_vfmul_vf_f32m1(pow2(oct, vl), 131071.0f, vl);
    vf g = __riscv_vfdiv_vv_f32m1(__riscv_vfcvt_f_x_v_f32m1(mant, vl), den, vl);
    g = self(__riscv_vmsge_vx_i32m1_b32(x, 0xFFFFF, vl), splatf(0.0f, vl), g, vl);
    return self(__riscv_vmslt_vx_i32m1_b32(x, 0, vl), splatf(1.0f, vl), g, vl);
}

/// Lane access to the voices: a field at its offset in each lane's voice (base + at).
inline vu idx(vu at, std::uint32_t off, std::size_t vl) { return __riscv_vadd_vx_u32m1(at, off, vl); }
inline vi vgi(const std::uint8_t* base, vu at, std::uint32_t off, std::size_t vl) {
    return __riscv_vluxei32_v_i32m1(reinterpret_cast<const std::int32_t*>(base), idx(at, off, vl), vl);
}
inline vf vgf(const std::uint8_t* base, vu at, std::uint32_t off, std::size_t vl) {
    return __riscv_vluxei32_v_f32m1(reinterpret_cast<const float*>(base), idx(at, off, vl), vl);
}
inline void vput(std::uint8_t* base, vu at, vb keep, std::uint32_t off, vi v, std::size_t vl) {
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off, vl), v, vl);
}
inline void vput(std::uint8_t* base, vu at, vb keep, std::uint32_t off, vf v, std::size_t vl) {
    __riscv_vsuxei32_v_f32m1_m(keep, reinterpret_cast<float*>(base), idx(at, off, vl), v, vl);
}

/// One mod EG (mod_tick and mod_advance): its new state, position and delay counter.
inline void mod_eg(const std::uint8_t* base, vu at, const FieldsLE::Eg& e, vb gated, vi& state, vi& pos, vi& delay, std::size_t vl) {
    const auto Vi = [&](std::uint32_t off) { return vgi(base, at, off, vl); };
    const vi stt = Vi(e.state);
    pos = Vi(e.pos);
    delay = Vi(e.delay);
    const vi l1 = Vi(e.level1), l2 = Vi(e.level2), l3 = Vi(e.level3);
    // state 0: the delay
    const vb s0 = eqx(stt, 0, vl);
    vb ddone;
    const vi dnew = step_up(delay, Vi(e.delay_target), kBlockLE, ddone, vl);
    delay = sel(s0, dnew, delay, vl);
    // states 1 / 3 / 5 / 7 / 8: one step of pos, its target and step by state
    const vb s1 = eqx(stt, 1, vl), s3 = eqx(stt, 3, vl), s5 = eqx(stt, 5, vl), s7 = eqx(stt, 7, vl), s8 = eqx(stt, 8, vl);
    const vi target = sel(s1, l1, sel(s3, l2, sel(s8, Vi(e.release_level), l3, vl), vl), vl);
    const vi inc = sel(s1, Vi(e.inc0), sel(s3, Vi(e.inc1), sel(s5, Vi(e.inc2), sel(s7, Vi(e.inc_sustain), Vi(e.inc_release), vl), vl), vl), vl);
    const vb moving = __riscv_vmor_mm_b32(__riscv_vmor_mm_b32(s1, s3, vl), __riscv_vmor_mm_b32(__riscv_vmor_mm_b32(s5, s7, vl), s8, vl), vl);
    vb pdone;
    const vi pnew = step(pos, target, inc, pdone, vl);
    pos = sel(moving, pnew, pos, vl);
    // advance: a done delay or stage (1 / 3 / 5), the gate open
    const vb staged = __riscv_vmand_mm_b32(__riscv_vmor_mm_b32(s1, __riscv_vmor_mm_b32(s3, s5, vl), vl), pdone, vl);
    const vb adv = __riscv_vmandn_mm_b32(__riscv_vmor_mm_b32(__riscv_vmand_mm_b32(s0, ddone, vl), staged, vl), gated, vl);
    // mod_advance: the first later level that differs from where pos is
    const vb le3 = __riscv_vmsle_vx_i32m1_b32(stt, 3, vl), le1 = __riscv_vmsle_vx_i32m1_b32(stt, 1, vl);
    vi ns = sel(__riscv_vmand_mm_b32(le3, __riscv_vmsne_vv_i32m1_b32(pos, l3, vl), vl), splat(5, vl), splat(7, vl), vl);
    ns = sel(__riscv_vmand_mm_b32(le1, __riscv_vmsne_vv_i32m1_b32(pos, l2, vl), vl), splat(3, vl), ns, vl);
    ns = sel(__riscv_vmand_mm_b32(s0, __riscv_vmsne_vv_i32m1_b32(pos, l1, vl), vl), splat(1, vl), ns, vl);
    state = sel(adv, ns, stt, vl);
}

} // namespace

void control_le(LanesLE& L, const FieldsLE& F, const ControlTablesLE& t) {
    const std::size_t n = L.n;
    for (std::size_t k = 0, vl; k < n; k += vl) {
        vl = __riscv_vsetvl_e32m1(n - k);
        const vu at = __riscv_vle32_v_u32m1(L.voice + k, vl);
        std::uint8_t* const base = L.base;
        const auto Vi = [&](std::uint32_t off) { return vgi(base, at, off, vl); };
        const auto Vf = [&](std::uint32_t off) { return vgf(base, at, off, vl); };
        vb flag = __riscv_vmclr_m_b32(vl);              // lanes leaving the steady states
        const vb gated = __riscv_vmsne_vx_i32m1_b32(ld(L.gated + k, vl), 0, vl);
        const vf one = splatf(1.0f, vl), mone = splatf(-1.0f, vl), half = splatf(0.5f, vl), zero = splatf(0.0f, vl);

        // ── LFO (running: the fade factor 1) ─────────────────────────────────────────────────────
        vf amod = Vf(F.lfo_amod);
        {
            const vf tgt = ldf(L.amod_target + k, vl);
            const float glide = static_cast<float>(t.inc_sustain_glide);
            const vf upv = __riscv_vfmin_vv_f32m1(__riscv_vfadd_vf_f32m1(amod, glide, vl), tgt, vl);
            const vf dnv = __riscv_vfmax_vv_f32m1(__riscv_vfsub_vf_f32m1(amod, glide, vl), tgt, vl);
            amod = self(__riscv_vmflt_vv_f32m1_b32(amod, tgt, vl), upv, self(__riscv_vmflt_vv_f32m1_b32(tgt, amod, vl), dnv, amod, vl), vl);
        }
        // the depths: element + controller (integer-valued floats below 2^24: the reference's double sums
        // are exact, so are these), clamped
        const vf dp = __riscv_vfmin_vf_f32m1(__riscv_vfmax_vf_f32m1(__riscv_vfadd_vv_f32m1(Vf(F.lfo_pitch_depth), ldf(L.pmod + k, vl), vl), 0.0f, vl), 21845.0f, vl);
        const vf da = __riscv_vfmin_vf_f32m1(__riscv_vfmax_vf_f32m1(__riscv_vfadd_vv_f32m1(Vf(F.lfo_amp_depth), amod, vl), 0.0f, vl), static_cast<float>(128 << 12), vl);
        const vf df = __riscv_vfmin_vf_f32m1(__riscv_vfmax_vf_f32m1(__riscv_vfadd_vv_f32m1(Vf(F.lfo_filter_depth), ldf(L.fmod + k, vl), vl), 0.0f, vl), 262144.0f, vl);
        const vi w = Vi(F.lfo_wave), inv = Vi(F.lfo_invert);
        const vu ph = __riscv_vreinterpret_v_i32m1_u32m1(Vi(F.lfo_phase));
        const vf sgn_p = self(__riscv_vmsne_vx_i32m1_b32(__riscv_vand_vx_i32m1(inv, 4, vl), 0, vl), mone, one, vl);
        const vf sgn_a = self(__riscv_vmsne_vx_i32m1_b32(__riscv_vand_vx_i32m1(inv, 2, vl), 0, vl), mone, one, vl);
        const vf sgn_f = self(__riscv_vmsne_vx_i32m1_b32(__riscv_vand_vx_i32m1(inv, 1, vl), 0, vl), mone, one, vl);
        const vb w0 = eqx(w, 0, vl), w1 = eqx(w, 1, vl), w5 = eqx(w, 5, vl), w6 = eqx(w, 6, vl), w7 = eqx(w, 7, vl), w8 = eqx(w, 8, vl);
        const vb is_saw = __riscv_vmor_mm_b32(__riscv_vmor_mm_b32(w0, w5, vl), eqx(w, 9, vl), vl);
        const vb is_tri = __riscv_vmor_mm_b32(__riscv_vmor_mm_b32(w1, w6, vl), eqx(w, 10, vl), vl);
        const vb is_sq = __riscv_vmor_mm_b32(__riscv_vmor_mm_b32(eqx(w, 2, vl), w7, vl), eqx(w, 11, vl), vl);
        vf wp, wa, wf;
        {   // saw: (int((ph + off) & 0xFFFFF) - 0x80000) / 2^20
            const auto saw = [&](vu off) {
                const vi u = __riscv_vreinterpret_v_u32m1_i32m1(__riscv_vand_vx_u32m1(__riscv_vadd_vv_u32m1(ph, off, vl), 0xFFFFF, vl));
                return __riscv_vfmul_vf_f32m1(__riscv_vfcvt_f_x_v_f32m1(__riscv_vsub_vx_i32m1(u, 0x80000, vl), vl), 1.0f / 1048576.0f, vl);
            };
            const vf so = saw(__riscv_vreinterpret_v_i32m1_u32m1(sel(w0, splat(0x80000, vl), splat(0, vl), vl)));
            wp = __riscv_vfmul_vv_f32m1(sgn_p, saw(__riscv_vreinterpret_v_i32m1_u32m1(splat(0x80000, vl))), vl);
            wa = __riscv_vfadd_vf_f32m1(__riscv_vfmul_vv_f32m1(self(w0, __riscv_vfneg_v_f32m1(sgn_a, vl), sgn_a, vl), so, vl), 0.5f, vl);
            wf = __riscv_vfadd_vv_f32m1(__riscv_vfmul_vv_f32m1(sgn_f, so, vl), self(w5, half, zero, vl), vl);
        }
        {   // triangle: u = (ph + off) & 0xFFFFF, folded; (int(u) - 0x40000) / 2^19
            const auto tri = [&](vu off) {
                vu u = __riscv_vand_vx_u32m1(__riscv_vadd_vv_u32m1(ph, off, vl), 0xFFFFF, vl);
                const vb hi = __riscv_vmsne_vx_u32m1_b32(__riscv_vand_vx_u32m1(u, 0x80000, vl), 0, vl);
                u = __riscv_vxor_vx_u32m1_mu(hi, u, u, 0xFFFFF, vl);
                return __riscv_vfmul_vf_f32m1(__riscv_vfcvt_f_x_v_f32m1(__riscv_vsub_vx_i32m1(__riscv_vreinterpret_v_u32m1_i32m1(u), 0x40000, vl), vl), 1.0f / 524288.0f, vl);
            };
            const vf to = tri(__riscv_vreinterpret_v_i32m1_u32m1(sel(w1, splat(0x40000, vl), splat(0, vl), vl)));
            const vf tp = __riscv_vfmul_vv_f32m1(sgn_p, tri(__riscv_vreinterpret_v_i32m1_u32m1(splat(0x40000, vl))), vl);
            const vf ta = __riscv_vfadd_vf_f32m1(__riscv_vfmul_vv_f32m1(self(w1, __riscv_vfneg_v_f32m1(sgn_a, vl), sgn_a, vl), to, vl), 0.5f, vl);
            const vf tf = __riscv_vfadd_vv_f32m1(__riscv_vfmul_vv_f32m1(sgn_f, to, vl), self(w6, half, zero, vl), vl);
            wp = self(is_tri, tp, wp, vl), wa = self(is_tri, ta, wa, vl), wf = self(is_tri, tf, wf, vl);
        }
        {   // square: the half the phase is in
            const vb second = __riscv_vmsne_vx_u32m1_b32(__riscv_vand_vx_u32m1(ph, 0x80000, vl), 0, vl);
            const vb ia = __riscv_vmsne_vx_i32m1_b32(__riscv_vand_vx_i32m1(inv, 2, vl), 0, vl);
            const vb iff = __riscv_vmsne_vx_i32m1_b32(__riscv_vand_vx_i32m1(inv, 1, vl), 0, vl);
            const vf qp = __riscv_vfmul_vv_f32m1(self(second, splatf(-0.5f, vl), half, vl), sgn_p, vl);
            const vf qa = self(__riscv_vmxor_mm_b32(second, ia, vl), one, zero, vl);
            const vf qf = self(w7, self(__riscv_vmxor_mm_b32(second, iff, vl), one, zero, vl),
                               __riscv_vfmul_vv_f32m1(self(second, half, splatf(-0.5f, vl), vl), sgn_f, vl), vl);
            wp = self(is_sq, qp, wp, vl), wa = self(is_sq, qa, wa, vl), wf = self(is_sq, qf, wf, vl);
        }
        {   // S&H: the value drawn (begin)
            const vf sh = Vf(F.lfo_sh);
            const vf hp = __riscv_vfmul_vv_f32m1(__riscv_vfmul_vf_f32m1(sh, 0.5f, vl), sgn_p, vl);
            const vf ha = __riscv_vfrsub_vf_f32m1(__riscv_vfmul_vv_f32m1(__riscv_vfmul_vf_f32m1(sh, 0.5f, vl), sgn_a, vl), 0.5f, vl);
            const vf hf = __riscv_vfadd_vv_f32m1(__riscv_vfmul_vv_f32m1(__riscv_vfmul_vf_f32m1(sh, -0.5f, vl), sgn_f, vl), self(w8, half, zero, vl), vl);
            const vb is_sh = __riscv_vmnor_mm_b32(__riscv_vmor_mm_b32(is_saw, is_tri, vl), is_sq, vl);
            wp = self(is_sh, hp, wp, vl), wa = self(is_sh, ha, wa, vl), wf = self(is_sh, hf, wf, vl);
        }
        const vi out_pitch = ftol_mul(dp, wp, vl);
        const vi out_amp = ftol_mul(da, wa, vl);
        const vi out_filter = ftol_mul(df, wf, vl);
        const vb biased = __riscv_vmnor_mm_b32(__riscv_vmand_mm_b32(__riscv_vmsge_vx_i32m1_b32(w, 5, vl), __riscv_vmsle_vx_i32m1_b32(w, 8, vl), vl), eqx(w, 12, vl), vl);
        const vi out_bias = sel(biased, trunc_f(__riscv_vfmul_vf_f32m1(df, 0.5f, vl), vl), splat(0, vl), vl);
        const vu next = __riscv_vand_vx_u32m1(__riscv_vadd_vv_u32m1(ph, __riscv_vreinterpret_v_i32m1_u32m1(Vi(F.lfo_inc)), vl), 0xFFFFF, vl);
        const vb quad = __riscv_vmsne_vx_u32m1_b32(__riscv_vand_vx_u32m1(__riscv_vxor_vv_u32m1(next, ph, vl), 0xC0000, vl), 0, vl);
        const vi sh_flag = sel(quad, splat(1, vl), ld(L.sh_flag + k, vl), vl);

        // ── pitch EG, portamento, the phase increment ────────────────────────────────────────────
        vi peg_state, peg_pos, peg_delay;
        mod_eg(base, at, F.eg[0], gated, peg_state, peg_pos, peg_delay, vl);
        vb d;
        const vi porta = step(Vi(F.porta_pos), splat(0, vl), Vi(F.porta_step), d, vl);
        const vi pitch_base = ld(L.pitch_base + k, vl);
        vi inc;
        {
            vi b = __riscv_vmax_vx_i32m1(__riscv_vmin_vx_i32m1(__riscv_vadd_vv_i32m1(pitch_base, porta, vl), 0x80000, vl), -0x80000, vl);
            const vi tune = sel(__riscv_vmslt_vx_i32m1_b32(Vi(F.pos_hi), 0, vl), splat(0, vl), Vi(F.loop_tune), vl);
            b = __riscv_vadd_vv_i32m1(__riscv_vadd_vv_i32m1(__riscv_vadd_vv_i32m1(b, peg_pos, vl), out_pitch, vl), tune, vl);
            b = __riscv_vmax_vx_i32m1(__riscv_vmin_vx_i32m1(b, 0x80000, vl), -0x80000, vl);
            const vi v = __riscv_vsra_vx_i32m1(b, 6, vl);
            const vu off = __riscv_vsll_vx_u32m1(__riscv_vreinterpret_v_i32m1_u32m1(__riscv_vand_vx_i32m1(v, 0x3FF, vl)), 2, vl);
            const vf mant = __riscv_vluxei32_v_f32m1(t.pitch, off, vl);
            inc = trunc_f(__riscv_vfmul_vv_f32m1(mant, pow2(__riscv_vadd_vx_i32m1(__riscv_vsra_vx_i32m1(v, 10, vl), 15, vl), vl), vl), vl);
        }

        // ── filter EG, cutoff glide, the coefficient ─────────────────────────────────────────────
        vi feg_state, feg_pos, feg_delay;
        mod_eg(base, at, F.eg[1], gated, feg_state, feg_pos, feg_delay, vl);
        const vi cutoff_target = ld(L.cutoff_target + k, vl);
        const vi cutoff = step(Vi(F.cutoff_cur), cutoff_target, splat(t.inc_cutoff_glide, vl), d, vl);
        vf f_new, f_prev;
        {
            vi lc = __riscv_vmax_vx_i32m1(__riscv_vmin_vx_i32m1(__riscv_vadd_vv_i32m1(cutoff, feg_pos, vl), 0x100000, vl), 0x80000, vl);
            lc = __riscv_vmax_vx_i32m1(__riscv_vmin_vx_i32m1(__riscv_vadd_vv_i32m1(lc, out_bias, vl), 0x100000, vl), 0x80000, vl);
            lc = __riscv_vmin_vv_i32m1(lc, ld(L.ceiling + k, vl), vl);
            lc = __riscv_vmax_vx_i32m1(__riscv_vsub_vv_i32m1(lc, out_filter, vl), 0, vl);
            const vi m = __riscv_vadd_vx_i32m1(__riscv_vand_vx_i32m1(__riscv_vsra_vx_i32m1(lc, 8, vl), 255, vl), 256, vl);
            f_new = __riscv_vfmin_vf_f32m1(__riscv_vfmul_vv_f32m1(__riscv_vfcvt_f_x_v_f32m1(m, vl),
                                                                   pow2(__riscv_vsub_vx_i32m1(__riscv_vsra_vx_i32m1(lc, 16, vl), 24, vl), vl), vl), 1.0f, vl);
            const vf fp = Vf(F.f_prev);
            f_prev = self(__riscv_vmflt_vf_f32m1_b32(fp, 0.0f, vl), f_new, fp, vl);
        }

        // ── amp EG ──────────────────────────────────────────────────────────────────────────────
        const vi stt = Vi(F.amp_state);
        vi x = Vi(F.amp_x), hold = Vi(F.amp_hold);
        const vf gain_prev = Vf(F.amp_gain);
        vf gain;
        vb fin;
        {
            const vb s0 = eqx(stt, 0, vl), s1 = eqx(stt, 1, vl), s2 = eqx(stt, 2, vl), s3 = eqx(stt, 3, vl), s4 = eqx(stt, 4, vl),
                     s5 = eqx(stt, 5, vl), s6 = eqx(stt, 6, vl), s7 = eqx(stt, 7, vl), s8 = eqx(stt, 8, vl), s9 = eqx(stt, 9, vl);
            // hold states 0 / 2 / 4 step the hold toward 0; the others step x
            const vb holding = __riscv_vmor_mm_b32(s0, __riscv_vmor_mm_b32(s2, s4, vl), vl);
            const vi inc1 = Vi(F.amp_inc1), inc2 = Vi(F.amp_inc2);
            const vi hinc = sel(s0, ld(L.amp_inc_attack + k, vl), sel(s2, inc1, inc2, vl), vl);
            vb hdone;
            const vi hnew = step(hold, splat(0, vl), hinc, hdone, vl);
            hold = sel(holding, hnew, hold, vl);
            const vi silent = splat(kSilent, vl);
            const vi target = sel(s1, Vi(F.amp_level1), sel(s3, Vi(F.amp_level2),
                                  sel(__riscv_vmor_mm_b32(s5, s7, vl), Vi(F.amp_level3), silent, vl), vl), vl);
            const vi xinc = sel(__riscv_vmor_mm_b32(s1, s9, vl), Vi(F.amp_xstep), sel(s3, inc1, sel(s5, inc2,
                                sel(s6, Vi(F.amp_inc3), sel(s7, splat(t.inc_sustain_glide, vl), Vi(F.amp_inc_release), vl), vl), vl), vl), vl);
            const vb xmoving = __riscv_vmor_mm_b32(__riscv_vmor_mm_b32(__riscv_vmor_mm_b32(s1, s3, vl), __riscv_vmor_mm_b32(s5, s6, vl), vl),
                                                   __riscv_vmor_mm_b32(__riscv_vmor_mm_b32(s7, s8, vl), s9, vl), vl);
            vb xdone;
            const vi xnew = step(x, target, xinc, xdone, vl);
            x = sel(xmoving, xnew, x, vl);
            // a stage end (states 0..5) leaves the steady states; so does an instant decay (rate 127)
            const vb staged = __riscv_vmor_mm_b32(__riscv_vmand_mm_b32(holding, hdone, vl),
                                                  __riscv_vmand_mm_b32(__riscv_vmor_mm_b32(s1, __riscv_vmor_mm_b32(s3, s5, vl), vl), xdone, vl), vl);
            const vb instant = __riscv_vmor_mm_b32(__riscv_vmand_mm_b32(s2, eqx(Vi(F.amp_decay1_rate), 127, vl), vl),
                                                   __riscv_vmand_mm_b32(s4, eqx(Vi(F.amp_decay2_rate), 127, vl), vl), vl);
            flag = __riscv_vmor_mm_b32(flag, __riscv_vmor_mm_b32(staged, instant, vl), vl);
            // the gain: linear stages from the line, the others by the law
            const vb linear = __riscv_vmor_mm_b32(s1, s9, vl);
            const vf line = __riscv_vfsub_vv_f32m1(Vf(F.amp_intercept), __riscv_vfmul_vv_f32m1(__riscv_vfcvt_f_x_v_f32m1(x, vl), Vf(F.amp_slope), vl), vl);
            gain = self(linear, line, eg_gain(x, vl), vl);
            fin = __riscv_vmor_mm_b32(__riscv_vmfle_vf_f32m1_b32(gain, 0.0f, vl), __riscv_vmand_mm_b32(s9, __riscv_vmsge_vx_i32m1_b32(x, kSilent, vl), vl), vl);
            gain = self(fin, zero, gain, vl);
        }
        const vi timer = Vi(F.peak_timer);
        const vb restart = __riscv_vmsge_vx_i32m1_b32(timer, 4096, vl);

        // ── the lanes the kernel keeps: everything written from here on, to them alone ───────────
        const vb keep = __riscv_vmandn_mm_b32(__riscv_vmsne_vx_i32m1_b32(ld(L.ok + k, vl), 0, vl), flag, vl);
        st(L.ok + k, sel(keep, splat(1, vl), splat(0, vl), vl), vl);
        vput(base, at, keep, F.lfo_amod, amod, vl);
        vput(base, at, keep, F.out_pitch, out_pitch, vl);
        vput(base, at, keep, F.out_amp, out_amp, vl);
        vput(base, at, keep, F.out_filter, out_filter, vl);
        vput(base, at, keep, F.out_bias, out_bias, vl);
        vput(base, at, keep, F.lfo_phase, __riscv_vreinterpret_v_u32m1_i32m1(next), vl);
        st(L.sh_flag + k, sh_flag, vl);
        vput(base, at, keep, F.eg[0].state, peg_state, vl);
        vput(base, at, keep, F.eg[0].pos, peg_pos, vl);
        vput(base, at, keep, F.eg[0].delay, peg_delay, vl);
        vput(base, at, keep, F.eg[1].state, feg_state, vl);
        vput(base, at, keep, F.eg[1].pos, feg_pos, vl);
        vput(base, at, keep, F.eg[1].delay, feg_delay, vl);
        vput(base, at, keep, F.porta_pos, porta, vl);
        vput(base, at, keep, F.pitch_base, pitch_base, vl);
        vput(base, at, keep, F.inc, inc, vl);
        vput(base, at, keep, F.cutoff_target, cutoff_target, vl);
        vput(base, at, keep, F.cutoff_cur, cutoff, vl);
        vput(base, at, keep, F.f_new, f_new, vl);
        vput(base, at, keep, F.f_prev, f_prev, vl);
        vput(base, at, keep, F.amp_x, x, vl);
        vput(base, at, keep, F.amp_hold, hold, vl);
        vput(base, at, keep, F.amp_gain_prev, gain_prev, vl);
        vput(base, at, keep, F.amp_gain, gain, vl);
        st(L.finished + k, sel(fin, splat(1, vl), splat(0, vl), vl), vl);
        vput(base, at, keep, F.stage_ticks, __riscv_vadd_vx_i32m1(Vi(F.stage_ticks), 1, vl), vl);
        vput(base, at, keep, F.peak_timer, sel(restart, splat(0, vl), __riscv_vadd_vx_i32m1(timer, kBlockLE, vl), vl), vl);
        st(L.peak_reset + k, sel(restart, splat(1, vl), splat(0, vl), vl), vl);

        // ── output stage: glides, gains, the mix ramps ───────────────────────────────────────────
        const vi level = step(Vi(F.level_x), ld(L.level_t + k, vl), splat(t.inc_sustain_glide, vl), d, vl);
        const vi pan0 = step(Vi(F.pan_x[0]), ld(L.pan_t[0] + k, vl), splat(t.inc_pan, vl), d, vl);
        const vi pan1 = step(Vi(F.pan_x[1]), ld(L.pan_t[1] + k, vl), splat(t.inc_pan, vl), d, vl);
        vput(base, at, keep, F.level_x, level, vl);
        vput(base, at, keep, F.pan_x[0], pan0, vl);
        vput(base, at, keep, F.pan_x[1], pan1, vl);
        const vb first = eqx(ld(L.mixed + k, vl), 0, vl);
        // one output's ramp: from the previous block's gain (its own on the first block) to this one's
        const auto emit = [&](std::size_t o, vf g) {
            const vf gp = self(first, g, Vf(F.gain_prev[o]), vl);
            vput(base, at, keep, F.mix_from[o], __riscv_vfmul_vf_f32m1(__riscv_vfmul_vv_f32m1(gain_prev, gp, vl), kBusScale, vl), vl);
            vput(base, at, keep, F.mix_to[o], __riscv_vfmul_vf_f32m1(__riscv_vfmul_vv_f32m1(gain, g, vl), kBusScale, vl), vl);
            vput(base, at, keep, F.gain_prev[o], g, vl);
        };
        for (std::size_t i = 0; i < 4; ++i) {
            const vi s = step(Vi(F.send_x[i]), ld(L.send_t[i] + k, vl), splat(t.inc_send, vl), d, vl);
            vput(base, at, keep, F.send_x[i], s, vl);
            const vi tt = __riscv_vadd_vv_i32m1(__riscv_vadd_vv_i32m1(s, level, vl), out_amp, vl);
            const vb mute = __riscv_vmsge_vx_i32m1_b32(tt, 0xFF000, vl);
            const vf gl = self(mute, zero, eg_gain(__riscv_vadd_vv_i32m1(pan0, tt, vl), vl), vl);
            const vf gr = self(mute, zero, eg_gain(__riscv_vadd_vv_i32m1(pan1, tt, vl), vl), vl);
            switch (i) {
            case 0: emit(0, gl); emit(1, gr); break;
            case 1: emit(2, __riscv_vfmul_vf_f32m1(__riscv_vfadd_vv_f32m1(gl, gr, vl), 0.5f, vl)); break;
            case 2: emit(3, gl); emit(4, gr); break;
            default: emit(5, gl); emit(6, gr); break;
            }
        }
    }
}

} // namespace hvyxg::accel::rvv

#endif
