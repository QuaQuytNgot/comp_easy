/*
 * real_example_v2.c  —  CTS Streaming Loop
 *
 * Demonstrates all three scheduling algorithms side-by-side.
 * Change ACTIVE_ALGORITHM to switch between them at compile time:
 *
 *   SCHEDULER_GREEDY  — baseline priority-greedy
 *   SCHEDULER_RA_MPC  — Risk-Aware MPC (minimises stalls over H-step horizon)
 *   SCHEDULER_SLR     — Stochastic Lagrangian Relaxation (adapts λ/μ/γ per seg)
 *
 * CTS Runtime Logic (Section V-D):
 *   1. resource_monitor_update()     → Δutime, Δstime, ρ_sys, C_proc
 *   2. vpes_legr()                   → predicted (yaw, pitch)
 *   3. build_probability_map()       → p_i(t) for all tiles
 *   4. cts_schedule()                → per-tile quality r_i via chosen algo
 *   5. request_handler_v2_post_get_info()
 *        • jobs[i].prob_ptr = &p_map[tile_id]
 *        • http_pool polling loop checks *prob_ptr < tau → RESET_STREAM
 *   6. SLR multiplier update is done inside run_slr() automatically.
 *
 * Cross-layer mapping:
 *   ρ_sys (CPU uncertainty)  ↔  Viewport uncertainty → drives λ_eff / μ
 *   Quality level change     ↔  DVS/DVFS step
 *   RESET_STREAM             ↔  Reclaim μ (CPU resource multiplier)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "proto_comp/define.h"
#include "proto_comp/abr.h"
#include "proto_comp/viewport_prediction.h"
#include "proto_comp/request_handler_v2.h"
#include "proto_comp/cts_scheduler.h"
#include "proto_comp/resource_monitor.h"

/* ── configuration ──────────────────────────────────────────────────────── */
#define SERVER_ADDR        "https://192.168.101.17:8443"
#define TOTAL_SEGMENTS     30
#define VP_HISTORY_SZ      20
#define PROTOCOL           STREAM_HTTP_3_0
#define TAU_EARLY_TERM     0.10f   /* early-term threshold τ (0=disable)    */
#define RHO_SYS_WARN       0.60f   /* print warning above this CPU pressure */

/* Select active algorithm: SCHEDULER_GREEDY | SCHEDULER_RA_MPC | SCHEDULER_SLR */
#define ACTIVE_ALGORITHM   SCHEDULER_SLR

/* ── probability map (Gaussian viewport model) ──────────────────────────── */
static void build_pmap(float yaw, float pitch, float *p, int n)
{
    const float sigma = VIEWPORT_WIDTH_DEGREES / 2.0f;
    const float two_s2 = 2.0f * sigma * sigma;
    for (int i = 0; i < n; i++) {
        int   row = i / NO_OF_COLS, col = i % NO_OF_COLS;
        float ty  = (col + 0.5f) * TILE_WIDTH  - 180.0f;
        float tp  = (row + 0.5f) * TILE_HEIGHT -  90.0f;
        float dy  = yaw - ty;
        if (dy >  180.0f) dy -= 360.0f;
        if (dy < -180.0f) dy += 360.0f;
        float dp  = pitch - tp;
        float pi  = expf(-(dy*dy + dp*dp) / two_s2);
        p[i] = (pi > 1.0f) ? 1.0f : (pi < 0.0f) ? 0.0f : pi;
    }
}

static int build_vp_list(const float *p, int n, int *vp, float thr)
{
    int k = 0;
    for (int i = 0; i < n; i++) if (p[i] >= thr) vp[k++] = i;
    return k;
}

/* ── harmonic bandwidth estimator ──────────────────────────────────────── */
#define BW_HIST 3
static float harmonic_bw(float *h, int n)
{
    float s = 0.0f; int c = 0;
    for (int i = 0; i < n; i++) if (h[i] > 0.0f) { s += 1.0f / h[i]; c++; }
    return (s > 0.0f && c > 0) ? ((float)c / s) : 1e6f;
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(void)
{
    const char *algo_name =
        (ACTIVE_ALGORITHM == SCHEDULER_RA_MPC) ? "RA-MPC" :
        (ACTIVE_ALGORITHM == SCHEDULER_SLR)    ? "SLR"    : "GREEDY";

    printf("=== CTS Streaming Demo  Algorithm: %s ===\n\n", algo_name);

    /* ── 1. Resource monitor ──────────────────────────────────────────── */
    resource_monitor_t rm;
    if (resource_monitor_init(&rm) != RET_SUCCESS) {
        fprintf(stderr, "[main] resource_monitor_init failed\n");
        return EXIT_FAILURE;
    }
    printf("[INIT] Resource monitor OK\n");

    /* ── 2. Viewport predictor (LEGR) ────────────────────────────────── */
    float yaw_h[VP_HISTORY_SZ], pit_h[VP_HISTORY_SZ];
    int   ts[VP_HISTORY_SZ];
    for (int i = 0; i < VP_HISTORY_SZ; i++) {
        yaw_h[i] = 180.0f; pit_h[i] = 0.0f; ts[i] = i;
    }
    viewport_prediction_t vpes;
    if (vpes_init(&vpes, yaw_h, pit_h, ts, 0, VP_HISTORY_SZ,
                  VP_HISTORY_SZ, VP_HISTORY_SZ,
                  VIEWPORT_ESTIMATOR_LEGR) != RET_SUCCESS) {
        fprintf(stderr, "[main] vpes_init failed\n");
        return EXIT_FAILURE;
    }
    printf("[INIT] Viewport predictor OK\n");

    /* ── 3. Request handler ──────────────────────────────────────────── */
    int tile_count = NO_OF_ROWS * NO_OF_COLS;
    request_handler_v2_t rh;
    if (request_handler_v2_init(&rh, SERVER_ADDR, TOTAL_SEGMENTS,
                                5, (COUNT)tile_count, PROTOCOL,
                                MAX_PARALLEL_VP_TILES) != RET_SUCCESS) {
        fprintf(stderr, "[main] rh init failed\n");
        return EXIT_FAILURE;
    }
    rh.pool->early_term_tau = TAU_EARLY_TERM;
    printf("[INIT] Request handler OK (tau=%.2f)\n", TAU_EARLY_TERM);

    /* ── 4. Scheduler structures ─────────────────────────────────────── */
    cts_tile_t  *tiles = (cts_tile_t *)calloc(tile_count, sizeof(cts_tile_t));
    if (!tiles) { request_handler_v2_destroy(&rh); return EXIT_FAILURE; }

    cts_output_t sched_out = { .tiles = tiles, .tile_count = tile_count };
    slr_state_t  slr_state;
    slr_state_init(&slr_state);

    float p_map[NO_OF_ROWS * NO_OF_COLS];
    int   vp_tiles[NO_OF_ROWS * NO_OF_COLS];
    float bw_hist[BW_HIST] = { 1e6f, 1e6f, 1e6f };
    int   bw_idx   = 0;
    float buf_level = MAX_BUFFER_SIZE / 2.0f;
    int   last_q    = 0;

    printf("[INIT] Ready.  Starting %d segments.\n\n", TOTAL_SEGMENTS);

    /* ════════════════════════════════════════════════════════════════════
     * SEGMENT LOOP
     * ════════════════════════════════════════════════════════════════════ */
    for (COUNT seg = 0; seg < TOTAL_SEGMENTS; seg++) {
        printf("━━━━━━  SEG %llu  buf=%.2fs  ━━━━━━\n", seg+1, buf_level);

        /* Step A: system resource sample */
        resource_monitor_update(&rm);
        float rho  = get_system_status(&rm);
        float cproc = resource_monitor_get_cproc(&rm);
        printf("[RM] rho_sys=%.4f  C_proc=%.1f%s\n",
               rho, cproc, (rho > RHO_SYS_WARN) ? "  [HIGH]" : "");

        /* Step B: viewport prediction */
        float pred_yaw = 180.0f, pred_pit = 0.0f;
        if (vpes.vpes_post(&vpes, &pred_yaw, &pred_pit) == RET_SUCCESS)
            printf("[VP] yaw=%.1f°  pitch=%.1f°\n", pred_yaw, pred_pit);
        else
            printf("[VP] prediction failed — using last centre\n");

        /* Step C: probability map + VP tile list */
        build_pmap(pred_yaw, pred_pit, p_map, tile_count);
        request_handler_v2_update_pmap(&rh, p_map);
        int nvp = build_vp_list(p_map, tile_count, vp_tiles, 0.5f);
        printf("[PM] %d viewport tiles\n", nvp);

        /* Step D: bandwidth estimate */
        float bw = harmonic_bw(bw_hist, BW_HIST);
        printf("[BW] %.2f Mbps\n", bw * 8.0f / 1e6f);

        /* Step E: CTS scheduling ─────────────────────────────────────── */
        /* λ or λ1 is also modulated by buffer pressure */
        float buf_pressure = (buf_level < B_HIGH)
                                 ? (1.0f - buf_level / B_HIGH) : 0.0f;

        cts_input_t cin;
        cts_input_init(&cin);
        cin.p_map         = p_map;
        cin.tile_count    = tile_count;
        cin.bandwidth     = bw;
        cin.buffer_level  = buf_level;
        cin.protocol      = PROTOCOL;
        cin.rho_sys       = rho;
        cin.c_proc_global = cproc;
        cin.last_quality  = last_q;

        /* SLR: when CPU load is high, CTS step 4 says reduce batch size.
         * We signal this by lowering early_term_tau temporarily. */
        if (ACTIVE_ALGORITHM == SCHEDULER_SLR && rho > RHO_SYS_WARN)
            rh.pool->early_term_tau = TAU_EARLY_TERM * 2.0f;
        else
            rh.pool->early_term_tau = TAU_EARLY_TERM;

        memset(&sched_out, 0, sizeof(sched_out));
        sched_out.tiles      = tiles;
        sched_out.tile_count = tile_count;

        if (cts_schedule(ACTIVE_ALGORITHM, &cin, &sched_out,
                         (ACTIVE_ALGORITHM == SCHEDULER_SLR) ? &slr_state
                                                              : NULL)
            != RET_SUCCESS) {
            fprintf(stderr, "[main] cts_schedule failed\n");
            break;
        }

        printf("[%s] J=%.2f  BW=%.2f Mbps  CPU=%.3f  VP_avg=%.0f bps\n",
               algo_name,
               sched_out.objective_value,
               sched_out.total_bw_used * 8.0f / 1e6f,
               sched_out.total_cpu_load,
               sched_out.vp_avg_quality);

        if (ACTIVE_ALGORITHM == SCHEDULER_SLR)
            printf("[SLR] λ=%.3f  μ=%.3f  γ=%.3f\n",
                   slr_state.lambda, slr_state.mu, slr_state.gamma);

        /* Record last chosen quality for smoothness penalty in next seg */
        if (nvp > 0) last_q = sched_out.tiles[vp_tiles[0]].chosen_version;

        /* Step F: parallel download with early termination */
        rh.cts_out = &sched_out;
        if (request_handler_v2_post_get_info(&rh, seg,
                                             vp_tiles, nvp,
                                             NULL, PROTOCOL)
            != RET_SUCCESS)
            fprintf(stderr, "[main] download seg %llu failed\n", seg+1);

        /* Step G: update bandwidth history from measured download speed */
        float tot_bytes = 0.0f, tot_us = 0.0f;
        for (int i = 0; i < nvp; i++) {
            int tid = vp_tiles[i];
            tot_bytes += (float)rh.size_dl[tid];
            tot_us    += (float)rh.total_time[tid];
        }
        float meas_bw = (tot_us > 0.0f) ? (tot_bytes / (tot_us / 1e6f)) : bw;
        bw_hist[bw_idx++ % BW_HIST] = meas_bw;

        /* Step H: simulate buffer update */
        float dl_time = (bw > 0.0f) ? (sched_out.vp_avg_quality * nvp / bw) : 0.5f;
        buf_level = (buf_level >= dl_time) ? (buf_level - dl_time) : 0.0f;
        buf_level += SEGMENT_DURATION;
        if (buf_level > MAX_BUFFER_SIZE) buf_level = MAX_BUFFER_SIZE;

        /* Step I: add simulated HMD sample */
        float sim_yaw = fmodf(180.0f + (float)seg * 3.5f, 360.0f);
        float sim_pit = fminf((float)seg * 0.8f, 25.0f);
        add_viewport_sample(&vpes, sim_yaw, sim_pit);

        request_handler_v2_reset(&rh);
    }

    /* ── shutdown ────────────────────────────────────────────────────────── */
    printf("\n=== Session complete  Algorithm: %s ===\n", algo_name);
    printf("  Total early-terminated tiles : %d\n", rh.early_term_count);
    if (ACTIVE_ALGORITHM == SCHEDULER_SLR)
        printf("  Final SLR multipliers: λ=%.4f  μ=%.4f  γ=%.4f\n",
               slr_state.lambda, slr_state.mu, slr_state.gamma);

    free(tiles);
    request_handler_v2_destroy(&rh);
    return EXIT_SUCCESS;
}