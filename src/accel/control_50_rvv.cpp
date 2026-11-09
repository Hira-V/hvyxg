// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// The S-YXG50 chip voice's control across lanes in RVV (control_50.hpp): RVV 1.0 (the X280) and
// XTheadVector (the C920) from one source, the voices' fields as runs (lanes_rvv.hpp). Exact against
// syxg50::ChipVoice::control: its integer glides and steps as the reference's, its float operations
// unfused in its order, the pitch LFO's 64-bit product at e64, lin's power of two exact. In phases (a
// loop over the lanes each, few registers live): the AEG, the LFO with pitch and cutoff, the levels
// with the gains and the mix ramps.

#if defined(HVYXG_ACCEL_RVV) && defined(HVYXG_CONTROL_50) && (defined(__riscv_vector) || defined(__riscv_xtheadvector))

#include "accel/control_50.hpp"
#include "accel/lanes_rvv.hpp"

#include <cstddef>
#include <cstdint>

namespace hvyxg::accel::rvv {

namespace {

constexpr std::int32_t kAttenOff = 0xfffff;

/// The S-YXG50's glide (chip.cpp): cur toward target by step; done once there (landing exactly on the
/// target reports not done).
inline vi glide(vi cur, vi target, vi step, vb& done, std::size_t vl) {
    const vb eq = __riscv_vmseq_vv_i32m1_b32(cur, target, vl);
    const vb gt = __riscv_vmslt_vv_i32m1_b32(target, cur, vl);
    const vi dn = __riscv_vsub_vv_i32m1(cur, step, vl);
    const vi up = __riscv_vadd_vv_i32m1(cur, step, vl);
    const vb dn_on = __riscv_vmand_mm_b32(gt, __riscv_vmsge_vv_i32m1_b32(dn, target, vl), vl);          // still above
    const vb up_on = __riscv_vmandn_mm_b32(__riscv_vmsle_vv_i32m1_b32(up, target, vl), __riscv_vmor_mm_b32(gt, eq, vl), vl);   // still below
    vi r = sel(gt, dn, up, vl);
    const vb on = __riscv_vmor_mm_b32(dn_on, up_on, vl);
    r = sel(on, r, target, vl);
    r = sel(eq, cur, r, vl);
    done = __riscv_vmnot_m_b32(on, vl);
    return r;
}

/// lin with the registers' "off" (0xfffff) giving 0.
inline vf lin_on(vi reg, vi x, std::size_t vl) { return self(eqx(reg, kAttenOff, vl), splatf(0.0f, vl), eg_gain(x, vl), vl); }

/// A 4-entry LFO quadrant table's word for each lane's phase (table[phase >> 18]).
inline vu quadrant(const std::uint32_t* table, vu ph, std::size_t vl) {
    return __riscv_vluxei32_v_u32m1(table, __riscv_vsll_vx_u32m1(__riscv_vsrl_vx_u32m1(ph, 18, vl), 2, vl), vl);
}

} // namespace

void control_50(Lanes50& L, const Fields50& F, const ControlTables50& t) {
    const std::size_t n = L.n;
    std::uint8_t* const base = L.base;

    // ── AEG ─────────────────────────────────────────────────────────────────────────────────────
    for (std::size_t k = 0, vl; k < n; k += vl) {
        vl = __riscv_vsetvl_e32m1(n - k);
        const vu at = __riscv_vle32_v_u32m1(L.voice + k, vl);
        const vb all = __riscv_vmset_m_b32(vl);
        vi stt, x, hold, init_level, target, step_a, step_d, reached_old;
        load_run(base, at, F.aeg_a, vl, stt, x, hold, init_level, target, step_a, step_d, reached_old);
        vi envp_i, env_i, slope_i, offset_i;
        load_run(base, at, F.aeg_b, vl, envp_i, env_i, slope_i, offset_i);
        const vf env_prev = asf(env_i);                       // env_prev = env
        const vb s0 = eqx(stt, 0, vl), s1 = eqx(stt, 1, vl), s2 = eqx(stt, 2, vl), s3 = eqx(stt, 3, vl), s4 = eqx(stt, 4, vl);
        // states 0 / 2 / 3 / 4 glide x (toward 0, init_level, target, target), 1 glides the hold to 0
        const vb xs = __riscv_vmor_mm_b32(__riscv_vmor_mm_b32(s0, s2, vl), __riscv_vmor_mm_b32(s3, s4, vl), vl);
        const vi xt = sel(s0, splat(0, vl), sel(s2, init_level, target, vl), vl);
        const vi xstep = sel(__riscv_vmor_mm_b32(s0, s2, vl), step_a, step_d, vl);
        vb xdone, hdone;
        const vi xnew = glide(x, xt, xstep, xdone, vl);
        const vi hnew = glide(hold, splat(0, vl), step_a, hdone, vl);
        x = sel(xs, xnew, x, vl);
        hold = sel(s1, hnew, hold, vl);
        // 0 / 1 / 2 done: decay (state 3); 3 / 4: reached
        const vb to3 = __riscv_vmor_mm_b32(__riscv_vmand_mm_b32(__riscv_vmor_mm_b32(s0, s2, vl), xdone, vl), __riscv_vmand_mm_b32(s1, hdone, vl), vl);
        vi state = sel(to3, splat(3, vl), stt, vl);
        const vi reached = sel(__riscv_vmand_mm_b32(__riscv_vmor_mm_b32(s3, s4, vl), xdone, vl), splat(1, vl), splat(0, vl), vl);
        // the envelope: the line in 0 and 4, 1 in 1, the law in 2 and 3, as it was otherwise
        const vf line = __riscv_vfsub_vv_f32m1(asf(offset_i), __riscv_vfmul_vv_f32m1(__riscv_vfcvt_f_x_v_f32m1(x, vl), asf(slope_i), vl), vl);
        vf env = self(__riscv_vmor_mm_b32(s0, s4, vl), line, asf(env_i), vl);
        env = self(s1, splatf(1.0f, vl), env, vl);
        env = self(__riscv_vmor_mm_b32(s2, s3, vl), eg_gain(x, vl), env, vl);
        // -48 dB in states 2..4 (the state after the step): the voice ends
        const vb in24 = __riscv_vmand_mm_b32(__riscv_vmsge_vx_i32m1_b32(state, 2, vl), __riscv_vmsle_vx_i32m1_b32(state, 4, vl), vl);
        const vb end = __riscv_vmand_mm_b32(in24, __riscv_vmfle_vf_f32m1_b32(env, t.lin_m48, vl), vl);   // lin(0x80000) >= env
        const vi off = splat(kAttenOff, vl);
        x = sel(end, off, x, vl);
        target = sel(end, off, target, vl);
        init_level = sel(end, off, init_level, vl);
        env = self(end, splatf(0.0f, vl), env, vl);
        store_run(base, at, all, F.aeg_a, vl, state, x, hold, init_level, target, step_a, step_d, reached);
        store_run(base, at, all, F.aeg_b, vl, asi(env_prev), asi(env), slope_i, offset_i);
        st(L.done + k, sel(end, splat(1, vl), splat(0, vl), vl), vl);
        stf(L.env + k, env, vl);
        stf(L.env_prev + k, env_prev, vl);
    }

    // ── LFO, the pitch LFO's increment, the cutoff ──────────────────────────────────────────────────
    for (std::size_t k = 0, vl; k < n; k += vl) {
        vl = __riscv_vsetvl_e32m1(n - k);
        const vu at = __riscv_vle32_v_u32m1(L.voice + k, vl);
        const vb all = __riscv_vmset_m_b32(vl);
        vi phase_i, lfo_step, tri_i, amd, fmd, pmd, inc, inc15_old;
        load_run(base, at, F.lfo, vl, phase_i, lfo_step, tri_i, amd, fmd, pmd, inc, inc15_old);
        const vu ph = __riscv_vreinterpret_v_i32m1_u32m1(phase_i);
        const vb tri = __riscv_vmsne_vx_i32m1_b32(tri_i, 0, vl);
        const vi am = __riscv_vreinterpret_v_u32m1_i32m1(__riscv_vxor_vv_u32m1(
            __riscv_vmerge_vvm_u32m1(quadrant(t.lfo_saw_uni, ph, vl), quadrant(t.lfo_tri_uni, ph, vl), tri, vl), ph, vl));
        const vi pm = __riscv_vreinterpret_v_u32m1_i32m1(__riscv_vxor_vv_u32m1(
            __riscv_vmerge_vvm_u32m1(quadrant(t.lfo_saw, ph, vl), quadrant(t.lfo_tri, ph, vl), tri, vl), ph, vl));
        // inc15 = clamp(inc + ((((pmd · pm) >> shift) · inc) >> 11), 0, 2^22), the product at 64 bits
        const vi pp = __riscv_vmul_vv_i32m1(pmd, pm, vl);
        const vi sh = sel(tri, __riscv_vsra_vx_i32m1(pp, 18, vl), __riscv_vsra_vx_i32m1(pp, 19, vl), vl);
        vint64m2_t w = __riscv_vsra_vx_i64m2(__riscv_vwmul_vv_i64m2(sh, inc, vl), 11, vl);
        w = __riscv_vwadd_wv_i64m2(w, inc, vl);
        w = __riscv_vmin_vx_i64m2(__riscv_vmax_vx_i64m2(w, 0, vl), std::int64_t{1} << 22, vl);
        const vi inc15 = __riscv_vnsra_wx_i32m1(w, 0, vl);
        const vu next = __riscv_vand_vx_u32m1(__riscv_vadd_vv_u32m1(ph, __riscv_vreinterpret_v_i32m1_u32m1(lfo_step), vl), 0xfffff, vl);
        store_run(base, at, all, F.lfo, vl, __riscv_vreinterpret_v_u32m1_i32m1(next), lfo_step, tri_i, amd, fmd, pmd, inc, inc15);
        // the amplitude LFO's level term, for the gains
        const vi amf = __riscv_vmul_vv_i32m1(amd, am, vl);
        st(L.am_level + k, sel(tri, __riscv_vsra_vx_i32m1(amf, 5, vl), __riscv_vsra_vx_i32m1(amf, 6, vl), vl), vl);

        // cutoff: the glide, then svf_f of it less the (unipolar) filter LFO, remembered
        vi cur, tgt, rate, fv, fov_i, fprev_old, fnew_old;
        load_run(base, at, F.cut, vl, cur, tgt, rate, fv, fov_i, fprev_old, fnew_old);
        vb d;
        const vi cstep = __riscv_vluxei32_v_i32m1(t.decay, __riscv_vsll_vx_u32m1(__riscv_vreinterpret_v_i32m1_u32m1(rate), 2, vl), vl);
        cur = glide(cur, tgt, cstep, d, vl);
        const vi fm = __riscv_vmul_vv_i32m1(fmd, am, vl);
        const vi v = __riscv_vsub_vv_i32m1(cur, sel(tri, __riscv_vsra_vx_i32m1(fm, 4, vl), __riscv_vsra_vx_i32m1(fm, 5, vl), vl), vl);
        const vb same = __riscv_vmseq_vv_i32m1_b32(v, fv, vl);
        vf fnew = asf(fov_i);
        vb need = __riscv_vmnot_m_b32(same, vl);
        vi fv2 = fv, fov2 = fov_i;
        if (t.rate_44k) {
            // f = min(((v & 0xffff) | 0x10000) · 2^((v >> 16) − 24) / 255, 1.99), v ≥ 0
            const vi vp = __riscv_vmax_vx_i32m1(v, 0, vl);
            const vf m = __riscv_vfcvt_f_x_v_f32m1(__riscv_vor_vx_i32m1(__riscv_vand_vx_i32m1(vp, 0xffff, vl), 0x10000, vl), vl);
            const vf f44 = __riscv_vfdiv_vf_f32m1(__riscv_vfmul_vv_f32m1(m, pow2(__riscv_vsub_vx_i32m1(__riscv_vsra_vx_i32m1(vp, 16, vl), 24, vl), vl), vl), 255.0f, vl);
            const vf f = __riscv_vfmin_vf_f32m1(f44, 1.99f, vl);
            fnew = self(same, fnew, f, vl);
            fv2 = v;
            fov2 = asi(fnew);
            need = __riscv_vmclr_m_b32(vl);
        }
        store_run(base, at, all, F.cut, vl, cur, tgt, rate, fv2, fov2, fnew_old, asi(fnew));
        st(L.v + k, v, vl);
        st(L.need_f + k, sel(need, splat(1, vl), splat(0, vl), vl), vl);
    }

    // ── levels, the gains, the mix ramps ────────────────────────────────────────────────────────────
    for (std::size_t k = 0, vl; k < n; k += vl) {
        vl = __riscv_vsetvl_e32m1(n - k);
        const vu at = __riscv_vle32_v_u32m1(L.voice + k, vl);
        const vb all = __riscv_vmset_m_b32(vl);
        vi lc, lt, plc, plt, prc, prt, dry, rev;
        load_run(base, at, F.lev_a, vl, lc, lt, plc, plt, prc, prt, dry, rev);
        vi cho, var;
        load_run(base, at, F.lev_b, vl, cho, var);
        vi gn0, gn1, gn2, gn3, gn4, gn5, gn6;
        load_run(base, at, F.gains + 28, vl, gn0, gn1, gn2, gn3, gn4, gn5, gn6);
        vb d;
        const vi lstep = splat(t.level_step, vl);
        lc = glide(lc, lt, lstep, d, vl);
        plc = glide(plc, plt, lstep, d, vl);
        prc = glide(prc, prt, lstep, d, vl);
        store_run(base, at, all, F.lev_a, vl, lc, lt, plc, plt, prc, prt, dry, rev);
        const vi lv = __riscv_vadd_vv_i32m1(ld(L.am_level + k, vl), lc, vl);
        const auto sum3 = [&](vi a, vi b) { return __riscv_vadd_vv_i32m1(__riscv_vadd_vv_i32m1(a, b, vl), lv, vl); };
        const vf g0 = lin_on(dry, sum3(plc, dry), vl), g1 = lin_on(dry, sum3(dry, prc), vl);
        const vf g2 = lin_on(rev, __riscv_vadd_vv_i32m1(rev, lv, vl), vl);
        const vf g3 = lin_on(cho, sum3(plc, cho), vl), g4 = lin_on(cho, sum3(cho, prc), vl);
        const vf g5 = lin_on(var, sum3(plc, var), vl), g6 = lin_on(var, sum3(prc, var), vl);
        // g_prev = the old g_new; the ramps: g · envelope at the block's start and end
        const vf ep = ldf(L.env_prev + k, vl), e = ldf(L.env + k, vl);
        store_run(base, at, all, F.gains, vl, gn0, gn1, gn2, gn3, gn4, gn5, gn6);
        store_run(base, at, all, F.gains + 28, vl, asi(g0), asi(g1), asi(g2), asi(g3), asi(g4), asi(g5), asi(g6));
        const auto from = [&](vi gp) { return asi(__riscv_vfmul_vv_f32m1(asf(gp), ep, vl)); };
        const auto to = [&](vf g) { return asi(__riscv_vfmul_vv_f32m1(g, e, vl)); };
        store_run(base, at, all, F.gains + 56, vl, from(gn0), from(gn1), from(gn2), from(gn3), from(gn4), from(gn5), from(gn6));
        store_run(base, at, all, F.gains + 84, vl, to(g0), to(g1), to(g2), to(g3), to(g4), to(g5), to(g6));
    }
}

} // namespace hvyxg::accel::rvv

#endif
