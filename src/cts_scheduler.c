/*
 * cts_scheduler.c  —  Coordinated Tile Scheduler (CTS)
 *
 * Three algorithms share a single cts_schedule() entry point:
 *
 * ── GREEDY ──────────────────────────────────────────────────────────────────
 *   Phase 1 – QoE guarantee: reserve quality-0 bandwidth for all VP tiles
 *             (p_i ≥ CTS_P_VP_THRESHOLD).
 *   Phase 2 – Priority sort: rank by  (p_i · ΔQ_max) / C_proc  desc.
 *   Phase 3 – Greedy upgrade: walk sorted list; for each tile try raising
 *             quality one level at a time while marginal utility > 0 and
 *             BW + CPU budgets are not exceeded.
 *
 * ── RA-MPC ───────────────────────────────────────────────────────────────────
 *   Paper objective (Section V-A, minimise over horizon H):
 *     Σ_{k=t}^{t+H} [ −E[QoE_k]  +  λ1·|Δr_k|  +  λ2·Risk(B_k, τ) ]
 *
 *   Implementation: per-tile exhaustive search over quality levels.
 *   For each candidate quality v we simulate H buffer steps and compute
 *   the H-step score.  We pick the v that maximises the accumulated score.
 *   Budget (BW, CPU) is tracked across tiles; tiles are processed in
 *   descending probability order so high-value tiles get first pick.
 *
 *   Budget accounting: every tile consumes VIDEO_BIT_RATE[v] bytes/s.
 *   We pre-deduct the base (quality-0) cost for all tiles so Phase 3
 *   upgrades only need to account for the *incremental* BW delta.
 *
 * ── SLR ──────────────────────────────────────────────────────────────────────
 *   Paper Lagrangian (Section V-B, maximise):
 *     L = Σ p_i·Q(r_i)
 *         − λ·(Σ r_i  − B̂)
 *         − μ·(Σ C_proc_i − τ_CPU)
 *         − γ·Risk(B, τ)
 *
 *   Dual decomposition: each tile solved independently.
 *   Per-tile marginal:  ΔL_i(v) = (p_i − λ)·Q(v) − μ·C_proc(v)
 *
 *   After every segment a subgradient step updates λ, μ, γ so the
 *   multipliers converge to reflect actual resource pressure:
 *     λ ← max(0, λ + η·(Σr_i − B̂))          bandwidth slack
 *     μ ← max(0, μ + η·(Σcpu_i − τ_CPU))     CPU slack
 *     γ ← max(0, γ + η·Risk(B_actual, τ))    QoE risk
 *
 *   CTS step 4 (Section V-D): when μ spikes (CPU overload), the caller
 *   should reduce the download batch size.  We signal this via
 *   out->total_cpu_load > CTS_SLR_TAU_CPU.
 *
 * Bug-fixes vs previous version:
 *   1. RA-MPC: quality-0 base cost is now pre-deducted from both bw_left
 *      and cpu_left before the per-tile upgrade loop, so tiles that remain
 *      at quality 0 correctly consume bandwidth.
 *   2. aggregate_output: total_bw_used / total_cpu_load are always recomputed
 *      from the final chosen_version values (consistent for all algorithms).
 *      The SLR objective value (set inside run_slr) is preserved.
 */

#include "proto_comp/cts_scheduler.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ── internal helpers ───────────────────────────────────────────────────── */

static float proto_factor(HTTP_VERSION p)
{
    switch (p) {
    case STREAM_HTTP_1_1: return CTS_PROTO_FACTOR_H1;
    case STREAM_HTTP_2_0: return CTS_PROTO_FACTOR_H2;
    case STREAM_HTTP_3_0: return CTS_PROTO_FACTOR_H3;
    default:              return CTS_PROTO_FACTOR_H2;
    }
}

float cts_cproc(int v, HTTP_VERSION p, float cg)
{
    if (v < 0 || v >= CTS_NUM_QUALITIES) return cg;
    return ((float)VIDEO_BIT_RATE[v] / (float)VIDEO_BIT_RATE[0])
           * proto_factor(p) * cg;
}

float cts_load_cpu(int v, HTTP_VERSION p)
{
    if (v < 0 || v >= CTS_NUM_QUALITIES) return 0.0f;
    /* Normalise: HTTP/3 quality-4 = 1.0 */
    float max_load = (float)VIDEO_BIT_RATE[CTS_NUM_QUALITIES - 1]
                   * CTS_PROTO_FACTOR_H3;
    float load = ((float)VIDEO_BIT_RATE[v] * proto_factor(p)) / max_load;
    return (load > 1.0f) ? 1.0f : load;
}

/* Linear buffer-shortage risk: Risk(B, τ) = max(0, τ − B) */
static float buf_risk(float buffer_level, float threshold)
{
    float r = threshold - buffer_level;
    return (r > 0.0f) ? r : 0.0f;
}

/* Descending-priority comparator for qsort */
typedef struct { int idx; float pri; } ranked_t;
static int cmp_desc(const void *a, const void *b)
{
    float pa = ((const ranked_t *)a)->pri;
    float pb = ((const ranked_t *)b)->pri;
    return (pb > pa) ? 1 : (pb < pa) ? -1 : 0;
}

/* ── shared init ────────────────────────────────────────────────────────── */

void cts_input_init(cts_input_t *in)
{
    if (!in) return;
    memset(in, 0, sizeof(*in));
    in->protocol     = STREAM_HTTP_2_0;
    in->buffer_level = MAX_BUFFER_SIZE / 2.0f;
    in->last_quality = 0;
}

void slr_state_init(slr_state_t *s)
{
    if (!s) return;
    s->lambda = CTS_SLR_LAMBDA_INIT;
    s->mu     = CTS_SLR_MU_INIT;
    s->gamma  = CTS_SLR_GAMMA_INIT;
}

/* ─────────────────────────────────────────────────────────────────────────
 * GREEDY
 * ───────────────────────────────────────────────────────────────────────── */
static RET run_greedy(const cts_input_t *in, cts_output_t *out)
{
    int   N        = in->tile_count;
    float bw_left  = in->bandwidth;
    float cpu_left = 1.0f;

    /* Adaptive λ: higher CPU pressure → penalise non-VP waste more */
    float lambda_eff = 1.5f * (1.0f + in->rho_sys);

    /* Initialise all tiles at quality 0 */
    for (int i = 0; i < N; i++) {
        cts_tile_t *t    = &out->tiles[i];
        t->tile_id       = i;
        t->probability   = in->p_map[i];
        t->chosen_version = 0;
        t->quality_bps   = (float)VIDEO_BIT_RATE[0];
        t->c_proc        = cts_cproc(0, in->protocol, in->c_proc_global);
        t->load_cpu      = cts_load_cpu(0, in->protocol);
        t->net_utility   = 0.0f;
        t->early_terminated = 0;
    }

    /* Phase 1: reserve quality-0 BW for viewport tiles */
    for (int i = 0; i < N; i++) {
        if (in->p_map[i] < CTS_P_VP_THRESHOLD) continue;
        float bw_cost  = (float)VIDEO_BIT_RATE[0];
        float cpu_cost = cts_load_cpu(0, in->protocol);
        if (bw_left >= bw_cost && cpu_left >= cpu_cost) {
            bw_left  -= bw_cost;
            cpu_left -= cpu_cost;
        }
    }

    /* Phase 2: priority ranking  Priority_i = (p_i * ΔQ_max) / C_proc */
    ranked_t *rank = (ranked_t *)malloc(N * sizeof(ranked_t));
    if (!rank) return RET_FAIL;
    for (int i = 0; i < N; i++) {
        float dq = (float)(VIDEO_BIT_RATE[CTS_NUM_QUALITIES - 1]
                          - VIDEO_BIT_RATE[0]);
        float cp = (out->tiles[i].c_proc > 0.0f)
                       ? out->tiles[i].c_proc : 1.0f;
        rank[i].idx = i;
        rank[i].pri = (in->p_map[i] * dq) / cp;
    }
    qsort(rank, N, sizeof(ranked_t), cmp_desc);

    /* Phase 3: greedy quality upgrade */
    for (int r = 0; r < N; r++) {
        int        i   = rank[r].idx;
        cts_tile_t *t  = &out->tiles[i];
        int        cur = t->chosen_version;  /* starts at 0 */

        for (int v = cur + 1; v < CTS_NUM_QUALITIES; v++) {
            float dbw  = (float)(VIDEO_BIT_RATE[v] - VIDEO_BIT_RATE[cur]);
            float dcpu = cts_load_cpu(v, in->protocol)
                       - cts_load_cpu(cur, in->protocol);
            if (dbw > bw_left || dcpu > cpu_left) break;

            /* Marginal utility: p·ΔQ − λ·(1−p)·ΔC_proc > 0 */
            float dq     = (float)(VIDEO_BIT_RATE[v] - VIDEO_BIT_RATE[cur]);
            float dc     = cts_cproc(v,   in->protocol, in->c_proc_global)
                         - cts_cproc(cur, in->protocol, in->c_proc_global);
            float margin = t->probability * dq
                         - lambda_eff * (1.0f - t->probability) * dc;
            if (margin <= 0.0f) break;

            bw_left  -= dbw;
            cpu_left -= dcpu;
            cur       = v;
        }

        t->chosen_version = cur;
        t->quality_bps    = (float)VIDEO_BIT_RATE[cur];
        t->c_proc         = cts_cproc(cur, in->protocol, in->c_proc_global);
        t->load_cpu       = cts_load_cpu(cur, in->protocol);
        t->net_utility    = t->probability * t->quality_bps
                          - lambda_eff * (1.0f - t->probability) * t->c_proc;
    }
    free(rank);
    return RET_SUCCESS;
}

/* ─────────────────────────────────────────────────────────────────────────
 * RA-MPC  (Risk-Aware Model Predictive Control)
 *
 * For each tile we search all quality levels and score each by simulating
 * H buffer steps.  Score (maximise):
 *   S(v) = Σ_{k=0}^{H-1} [ p·Q(v)/1000  − λ1·|Q(v)−Q(prev_v)|/1000
 *                         − λ2·Risk(B_k, τ) ]
 *
 * Budget accounting (BUG-FIX from previous version):
 *   We pre-deduct quality-0 for every tile so even tiles that stay at
 *   quality 0 consume their fair share of bandwidth.  Upgrade selection
 *   only deducts the *incremental* cost (BW[best_v] − BW[0]).
 * ───────────────────────────────────────────────────────────────────────── */
static RET run_ra_mpc(const cts_input_t *in, cts_output_t *out)
{
    int   N        = in->tile_count;
    float bw_left  = in->bandwidth;
    float cpu_left = 1.0f;

    /* Initialise all tiles at quality 0 */
    for (int i = 0; i < N; i++) {
        cts_tile_t *t    = &out->tiles[i];
        t->tile_id       = i;
        t->probability   = in->p_map[i];
        t->chosen_version = 0;
        t->quality_bps   = (float)VIDEO_BIT_RATE[0];
        t->c_proc        = cts_cproc(0, in->protocol, in->c_proc_global);
        t->load_cpu      = cts_load_cpu(0, in->protocol);
        t->net_utility   = 0.0f;
        t->early_terminated = 0;
    }

    /* Pre-deduct quality-0 cost for ALL tiles (BUG-FIX) */
    {
        float base_bw  = (float)VIDEO_BIT_RATE[0] * (float)N;
        float base_cpu = cts_load_cpu(0, in->protocol) * (float)N;
        bw_left  -= base_bw;
        cpu_left -= base_cpu;
        if (bw_left  < 0.0f) bw_left  = 0.0f;
        if (cpu_left < 0.0f) cpu_left = 0.0f;
    }

    /* Process tiles in descending probability order */
    ranked_t *rank = (ranked_t *)malloc(N * sizeof(ranked_t));
    if (!rank) return RET_FAIL;
    for (int i = 0; i < N; i++) {
        rank[i].idx = i;
        rank[i].pri = in->p_map[i];
    }
    qsort(rank, N, sizeof(ranked_t), cmp_desc);

    int prev_quality = in->last_quality;

    for (int r = 0; r < N; r++) {
        int        i   = rank[r].idx;
        cts_tile_t *t  = &out->tiles[i];
        float      p   = t->probability;

        float best_score = -1e9f;
        int   best_v     = 0;  /* quality-0 is always feasible (pre-deducted) */

        for (int v = 0; v < CTS_NUM_QUALITIES; v++) {
            /* Incremental BW/CPU cost over quality-0 base */
            float dbw  = (float)(VIDEO_BIT_RATE[v] - VIDEO_BIT_RATE[0]);
            float dcpu = cts_load_cpu(v, in->protocol)
                       - cts_load_cpu(0, in->protocol);

            /* Check incremental budget (base already deducted) */
            if (v > 0 && (dbw > bw_left || dcpu > cpu_left)) continue;

            /* Simulate H buffer steps */
            float sim_buf   = in->buffer_level;
            float score_sum = 0.0f;
            int   prev_v    = prev_quality;

            for (int k = 0; k < CTS_MPC_HORIZON; k++) {
                /* Expected quality reward (normalised to kbps) */
                float eq = p * (float)VIDEO_BIT_RATE[v] / 1000.0f;

                /* Stability: penalise quality jumps */
                float dr = fabsf((float)VIDEO_BIT_RATE[v]
                                - (float)VIDEO_BIT_RATE[prev_v]) / 1000.0f;

                /* Simulate buffer: download takes (bitrate / available_bw) s */
                float avail_bw = in->bandwidth > 0.0f ? in->bandwidth : 1.0f;
                float dl_time  = (float)VIDEO_BIT_RATE[v] / avail_bw;
                if (sim_buf < dl_time)
                    sim_buf = 0.0f;
                else
                    sim_buf -= dl_time;
                sim_buf += SEGMENT_DURATION;
                if (sim_buf > MAX_BUFFER_SIZE) sim_buf = MAX_BUFFER_SIZE;

                float risk = buf_risk(sim_buf, CTS_MPC_BUF_RISK_LOW);

                /* H-step score contribution (maximisation form) */
                score_sum += eq
                           - CTS_MPC_LAMBDA1 * dr
                           - CTS_MPC_LAMBDA2 * risk;
                prev_v = v;
            }

            if (score_sum > best_score) {
                best_score = score_sum;
                best_v     = v;
            }
        }

        /* Deduct only the incremental upgrade cost */
        if (best_v > 0) {
            float dbw  = (float)(VIDEO_BIT_RATE[best_v] - VIDEO_BIT_RATE[0]);
            float dcpu = cts_load_cpu(best_v, in->protocol)
                       - cts_load_cpu(0, in->protocol);
            bw_left  -= dbw;
            cpu_left -= dcpu;
            if (bw_left  < 0.0f) bw_left  = 0.0f;
            if (cpu_left < 0.0f) cpu_left = 0.0f;
        }

        t->chosen_version = best_v;
        t->quality_bps    = (float)VIDEO_BIT_RATE[best_v];
        t->c_proc         = cts_cproc(best_v, in->protocol, in->c_proc_global);
        t->load_cpu       = cts_load_cpu(best_v, in->protocol);
        t->net_utility    = best_score;
        prev_quality      = best_v;
    }
    free(rank);
    return RET_SUCCESS;
}

/* ─────────────────────────────────────────────────────────────────────────
 * SLR  (Stochastic Lagrangian Relaxation)
 *
 * Dual decomposition: each tile solved independently.
 * Per-tile marginal:  ΔL_i(v) = (p_i − λ)·Q(v) − μ·C_proc(v)
 *
 * No hard BW/CPU feasibility check per tile (the dual variables λ and μ
 * implicitly enforce feasibility through the price mechanism — when the
 * system is over-budget, λ/μ grow and the optimiser selects lower qualities).
 *
 * Subgradient multiplier update after all tiles are assigned:
 *   λ ← clip[0, λ_max]( λ + η·(Σr_i − B̂) )
 *   μ ← clip[0, μ_max]( μ + η·(Σcpu_i − τ_CPU) )
 *   γ ← clip[0, γ_max]( γ + η·Risk(B_actual, τ) )
 * ───────────────────────────────────────────────────────────────────────── */
static RET run_slr(const cts_input_t *in, cts_output_t *out, slr_state_t *slr)
{
    if (!slr) return RET_FAIL;

    int N = in->tile_count;

    /* Per-tile dual decomposition */
    for (int i = 0; i < N; i++) {
        cts_tile_t *t  = &out->tiles[i];
        t->tile_id     = i;
        t->probability = in->p_map[i];
        t->early_terminated = 0;

        float p       = in->p_map[i];
        float best_dl = -1e30f;
        int   best_v  = 0;

        for (int v = 0; v < CTS_NUM_QUALITIES; v++) {
            float q  = (float)VIDEO_BIT_RATE[v];
            float cp = cts_cproc(v, in->protocol, in->c_proc_global);

            /* ΔL_i(v) = (p_i − λ)·Q(v) − μ·C_proc(v)
             * γ·Risk is a per-segment constant — does not affect argmax. */
            float dl = (p - slr->lambda) * q - slr->mu * cp;

            if (dl > best_dl) {
                best_dl = dl;
                best_v  = v;
            }
        }

        t->chosen_version = best_v;
        t->quality_bps    = (float)VIDEO_BIT_RATE[best_v];
        t->c_proc         = cts_cproc(best_v, in->protocol, in->c_proc_global);
        t->load_cpu       = cts_load_cpu(best_v, in->protocol);
        t->net_utility    = best_dl;
    }

    /* Aggregate primal sums for subgradient */
    float sum_r   = 0.0f, sum_cpu = 0.0f, sum_pq = 0.0f;
    for (int i = 0; i < N; i++) {
        sum_r   += out->tiles[i].quality_bps;
        sum_cpu += out->tiles[i].load_cpu;
        sum_pq  += in->p_map[i] * out->tiles[i].quality_bps;
    }

    float risk_val = buf_risk(in->buffer_level, CTS_SLR_BUF_RISK_THR);

    /* L = Σp_i·Q − λ·(Σr − B̂) − μ·(Σcpu − τ) − γ·Risk */
    out->objective_value = sum_pq
                         - slr->lambda * (sum_r   - in->bandwidth)
                         - slr->mu     * (sum_cpu - CTS_SLR_TAU_CPU)
                         - slr->gamma  * risk_val;

    /* ── Subgradient multiplier update ──────────────────────────────────── */
    float eta = CTS_SLR_STEP;
    slr->lambda += eta * (sum_r   - in->bandwidth);
    slr->mu     += eta * (sum_cpu - CTS_SLR_TAU_CPU);
    slr->gamma  += eta * risk_val;

    /* Dual feasibility: non-negative, capped to avoid divergence */
    if (slr->lambda < 0.0f) slr->lambda = 0.0f;
    if (slr->mu     < 0.0f) slr->mu     = 0.0f;
    if (slr->gamma  < 0.0f) slr->gamma  = 0.0f;
    if (slr->lambda > 20.0f) slr->lambda = 20.0f;
    if (slr->mu     > 10.0f) slr->mu     = 10.0f;
    if (slr->gamma  > 10.0f) slr->gamma  = 10.0f;

    return RET_SUCCESS;
}

/* ── aggregate_output ────────────────────────────────────────────────────
 * Recomputes total_bw_used, total_cpu_load, vp_avg_quality from the
 * final chosen_version values — consistent for all three algorithms.
 * Preserves objective_value if it was already set (SLR sets its own).
 * ───────────────────────────────────────────────────────────────────────── */
static void aggregate_output(const cts_input_t *in, cts_output_t *out,
                             float saved_obj)
{
    float sum_bw  = 0.0f, sum_cpu = 0.0f, sum_util = 0.0f;
    float vp_q    = 0.0f;
    int   vp_cnt  = 0;

    for (int i = 0; i < out->tile_count; i++) {
        cts_tile_t *t = &out->tiles[i];
        if (t->chosen_version < 0) t->chosen_version = 0;

        /* Refresh derived fields from final version */
        t->quality_bps = (float)VIDEO_BIT_RATE[t->chosen_version];
        t->c_proc      = cts_cproc(t->chosen_version, in->protocol,
                                   in->c_proc_global);
        t->load_cpu    = cts_load_cpu(t->chosen_version, in->protocol);

        sum_bw  += t->quality_bps;
        sum_cpu += t->load_cpu;
        sum_util += t->net_utility;

        if (in->p_map[i] >= CTS_P_VP_THRESHOLD) {
            vp_q += t->quality_bps;
            vp_cnt++;
        }
    }

    out->total_bw_used  = sum_bw;
    out->total_cpu_load = sum_cpu;
    out->vp_avg_quality = (vp_cnt > 0) ? (vp_q / (float)vp_cnt) : 0.0f;

    /* Restore saved objective if the algorithm set its own (SLR) */
    out->objective_value = (saved_obj != 0.0f) ? saved_obj : sum_util;
}

/* ── main dispatch ───────────────────────────────────────────────────────── */
RET cts_schedule(CTS_ALGORITHM      algo,
                 const cts_input_t *in,
                 cts_output_t      *out,
                 slr_state_t       *slr)
{
    if (!in || !out || !out->tiles || !in->p_map || in->tile_count <= 0)
        return RET_FAIL;

    out->tile_count      = in->tile_count;
    out->objective_value = 0.0f;

    RET ret;
    switch (algo) {
    case SCHEDULER_GREEDY:  ret = run_greedy(in, out);       break;
    case SCHEDULER_RA_MPC:  ret = run_ra_mpc(in, out);       break;
    case SCHEDULER_SLR:     ret = run_slr(in, out, slr);     break;
    default:                return RET_FAIL;
    }
    if (ret != RET_SUCCESS) return RET_FAIL;

    /* Save objective before aggregate_output recomputes it */
    float saved_obj = out->objective_value;
    aggregate_output(in, out, saved_obj);
    return RET_SUCCESS;
}

/* ── debug print ─────────────────────────────────────────────────────────── */
void cts_print_schedule(const cts_output_t *out, CTS_ALGORITHM algo)
{
    if (!out || !out->tiles) return;
    const char *name = (algo == SCHEDULER_RA_MPC) ? "RA-MPC" :
                       (algo == SCHEDULER_SLR)    ? "SLR"    : "GREEDY";
    printf("\n[CTS/%s] J=%.2f  BW=%.2f Mbps  CPU=%.3f  VP_avg=%.0f bps\n",
           name, out->objective_value,
           out->total_bw_used * 8.0f / 1e6f,
           out->total_cpu_load, out->vp_avg_quality);
    printf("  %-6s %-5s %-4s %-10s %-8s\n",
           "Tile","p_i","Ver","Qual(bps)","Utility");
    printf("  %-6s %-5s %-4s %-10s %-8s\n",
           "------","-----","----","----------","--------");
    for (int i = 0; i < out->tile_count; i++) {
        const cts_tile_t *t = &out->tiles[i];
        printf("  %-6d %-5.3f %-4d %-10.0f %-8.2f%s\n",
               t->tile_id, t->probability, t->chosen_version,
               t->quality_bps, t->net_utility,
               t->early_terminated ? " [ET]" : "");
    }
    printf("\n");
}