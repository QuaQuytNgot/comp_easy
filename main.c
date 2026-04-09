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
#define SERVER_ADDR            "https://192.168.101.17:8443"
#define TOTAL_SEGMENTS         10
#define VP_HISTORY_SZ          20
#define PROTOCOL               STREAM_HTTP_3_0
#define TAU_EARLY_TERM         0.10f
#define RHO_SYS_WARN           0.60f
 
/* Half-extents of the viewport [degrees].  Viewport is 90×90°. */
#define VP_HALF_YAW            (VIEWPORT_WIDTH_DEGREES  / 2.0f)   /* 45° */
#define VP_HALF_PITCH          (VIEWPORT_HEIGHT_DEGREES / 2.0f)   /* 45° */
 
/* Saccade detection: if angular speed exceeds this (°/s) tau is zeroed. */
#define SACCADE_THRESHOLD_DEG_S  30.0f
 
/* Select active algorithm: SCHEDULER_RA_MPC, SCHEDULER_SLR */
#define ACTIVE_ALGORITHM        SCHEDULER_RA_MPC
 
typedef struct {
    float yaw;
    float pitch;
    int   timestamp;
} ViewportSample;
 
// ViewportSample viewport_dataset[] = {
//     {180.0f,  0.0f,  0},
//     {180.0f,  0.0f,  1000}, {180.0f, 0.0f, 2000}, {180.0f, 0.0f, 3000}, {180.0f, 0.0f, 4000},
//     {210.0f,  5.0f,  5000},
//     {270.0f, 15.0f,  6000},   /* sudden saccade — 60° in 1 s */
//     {280.0f, 10.0f,  7000},
//     {280.0f, 10.0f,  8000},
//     {280.0f, 10.0f,  9000},
// };

ViewportSample viewport_dataset[] = {
    {180.0f,  0.0f,  0},
    {192.0f,  3.0f,  1000}, {198.0f, 9.0f, 2000}, {177.0f, 12.0f, 3000}, {180.0f, 13.0f, 4000},
    {210.0f,  5.0f,  5000},
    {270.0f, 15.0f,  6000},   /* sudden saccade — 60° in 1 s */
    {280.0f, 10.0f,  7000},
    {210.0f, 10.0f,  8000},
    {280.0f, 10.0f,  9000},
};
 
/* ── [FIX-1 + FIX-2]  Top-Hat Gaussian probability map ─────────────────── *
 *
 * Model:
 *   p = 1.0                          if  dy ≤ VP_HALF_YAW  &&  dp ≤ VP_HALF_PITCH
 *   p = exp(-(excess²) / two_s2)    otherwise,
 *         where excess = distance from tile centre to the nearest viewport edge
 *
 * Tile centre coordinates (unified [0,360) / [-90,90]):
 *   ty = (col + 0.5) * TILE_WIDTH           — always in [0, 360)
 *   tp = -90 + (row + 0.5) * TILE_HEIGHT    — always in [-90, 90]
 *
 * Shortest circular yaw distance:
 *   dy_raw = |norm_yaw - ty|
 *   dy     = (dy_raw > 180) ? 360 - dy_raw : dy_raw
 */
static void build_pmap_adaptive(float yaw, float pitch,
                                float *p,  int   n,
                                float sigma)
{
    const float two_s2 = 2.0f * sigma * sigma;
 
    /* Normalise predicted direction — same coordinate system as tile centres */
    float ny = wrap_angle_360(yaw);
    float np = clamp_pitch(pitch);
 
    for (int i = 0; i < n; i++) {
        int col = i % NO_OF_COLS;
        int row = i / NO_OF_COLS;
 
        /* [FIX-2] Tile centre in [0,360) × [-90,90] */
        float ty = (col + 0.5f) * TILE_WIDTH;
        float tp = -90.0f + (row + 0.5f) * TILE_HEIGHT;
 
        /* [FIX-2] Shortest circular yaw distance */
        float dy_raw = fabsf(ny - ty);
        float dy     = (dy_raw > 180.0f) ? (360.0f - dy_raw) : dy_raw;
        float dp     = fabsf(np - tp);
 
        /* [FIX-1] Top-Hat: tiles inside FOV get p = 1.0 */
        if (dy <= VP_HALF_YAW && dp <= VP_HALF_PITCH) {
            p[i] = 1.0f;
        } else {
            /* Gaussian decay measured from the viewport EDGE, not the centre.
             * This makes the falloff start at the border, not the centre,
             * so near-edge tiles stay close to 1.0 rather than ~0.6.       */
            float excess_y = (dy > VP_HALF_YAW)  ? (dy - VP_HALF_YAW)  : 0.0f;
            float excess_p = (dp > VP_HALF_PITCH) ? (dp - VP_HALF_PITCH) : 0.0f;
            p[i] = expf(-(excess_y * excess_y + excess_p * excess_p) / two_s2);
        }
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
 
/* ── main ───────────────────────────────────────────────────────────────── */
int main(void)
{
    const char *algo_name =
        (ACTIVE_ALGORITHM == SCHEDULER_RA_MPC) ? "RA-MPC" :
        (ACTIVE_ALGORITHM == SCHEDULER_SLR)    ? "SLR"    : "GREEDY";
 
    printf("=== CTS Streaming Demo  Algorithm: %s ===\n\n", algo_name);
 
    /* 1. Resource monitor */
    resource_monitor_t rm;
    if (resource_monitor_init(&rm) != RET_SUCCESS) {
        fprintf(stderr, "[main] resource_monitor_init failed\n");
        return EXIT_FAILURE;
    }
    printf("[INIT] Resource monitor OK\n");
 
    /* 2. Viewport predictor (LEGR) */
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
 
    /* 3. Request handler */
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
 
    /* 4. Scheduler structures */
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
 
    /* Track previous predicted yaw for velocity estimation */
    float prev_pred_yaw = 180.0f;
    int   prev_ts_ms    = 0;
 
    printf("[INIT] Ready.  Starting %d segments.\n\n", TOTAL_SEGMENTS);
 
    /* ════════════════════════════════════════════════════════════════════
     * SEGMENT LOOP
     * ════════════════════════════════════════════════════════════════════ */
    for (COUNT seg = 0; seg < TOTAL_SEGMENTS; seg++) {
        printf("━━━━━━  SEG %llu  buf=%.2fs  ━━━━━━\n", seg+1, buf_level);
 
        int data_idx = (seg < (sizeof(viewport_dataset)/sizeof(ViewportSample)))
                       ? (int)seg : 0;
        float actual_yaw   = viewport_dataset[data_idx].yaw;
        float actual_pitch = viewport_dataset[data_idx].pitch;
        int   actual_ts_ms = viewport_dataset[data_idx].timestamp;
 
        /* Step A: system resource sample */
        resource_monitor_update(&rm);
        float rho   = get_system_status(&rm);
        float cproc = resource_monitor_get_cproc(&rm);
        printf("[RM] rho_sys=%.4f  C_proc=%.1f%s\n",
               rho, cproc, (rho > RHO_SYS_WARN) ? "  [HIGH]" : "");
 
        /* Step B: viewport prediction */
        float pred_yaw = 180.0f, pred_pit = 0.0f;
        if (vpes.vpes_post(&vpes, &pred_yaw, &pred_pit) == RET_SUCCESS)
            printf("[VP] pred yaw=%.1f°  pitch=%.1f°\n", pred_yaw, pred_pit);
        else
            printf("[VP] prediction failed — using last centre\n");
 
        /* ── [FIX-3] Adaptive sigma & tau (saccade-safe) ─────────────── */
 
        /* Angular velocity of the predicted trajectory [°/s].
         * Use the shortest circular distance to handle wrap-around. */
        float dt_s = (actual_ts_ms > prev_ts_ms)
                     ? (actual_ts_ms - prev_ts_ms) / 1000.0f
                     : 1.0f;
        float d_yaw_raw = fabsf(pred_yaw - prev_pred_yaw);
        float d_yaw     = (d_yaw_raw > 180.0f) ? (360.0f - d_yaw_raw) : d_yaw_raw;
        float velocity  = d_yaw / dt_s;           /* °/s */
 
        /* Widen sigma during fast motion */
        float adaptive_sigma = (velocity > 20.0f)
                               ? (VIEWPORT_WIDTH_DEGREES * 0.5f)   /* 45° */
                               : (VIEWPORT_WIDTH_DEGREES * 0.3f);  /* 27° */
 
        /* Prediction error for tau base */
        float diff_y_raw = fabsf(pred_yaw - actual_yaw);
        float diff_y     = (diff_y_raw > 180.0f) ? (360.0f - diff_y_raw) : diff_y_raw;
        float diff_p     = fabsf(pred_pit - actual_pitch);
        float pred_err   = sqrtf(diff_y * diff_y + diff_p * diff_p);
 
        float tau_base    = expf(-(pred_err * pred_err)
                                          / (2.0f * adaptive_sigma * adaptive_sigma));
        float tau_resource = 1.0f + 0.05f * slr_state.mu + (0.01f * slr_state.lambda);;
        float tau_final    = tau_base * tau_resource;
 
        /* Clamp to safe range */
        if (tau_final < 0.02f) tau_final = 0.02f;
        if (tau_final > 0.35f) tau_final = 0.35f;
 
        /* [FIX-3] SACCADE GUARD — zero tau during rapid head rotation.
         * This is the key fix for QoE drops: when velocity exceeds the
         * threshold we cannot trust the prediction, so we must NOT terminate
         * any tile.  tau = 0 disables early termination entirely.           */
        if (velocity > SACCADE_THRESHOLD_DEG_S) {
            tau_final = 0.0f;
            printf("[TAU] Saccade detected (%.1f °/s) — tau forced to 0\n", velocity);
        }
 
        /* Warm-up: disable ET for first 4 segments (predictor has no history) */
        if (seg < 3) tau_final = 0.0f;
 
        /* [FIX-4] Apply tau — NOT overridden by a stray hardcoded value */
        rh.pool->early_term_tau = tau_final;
        printf("[TAU] velocity=%.1f°/s  sigma=%.1f°  tau=%.3f\n",
               velocity, adaptive_sigma, tau_final);
 
        /* Step C: probability map + VP tile list */
        build_pmap_adaptive(pred_yaw, pred_pit, p_map, tile_count, adaptive_sigma);
        request_handler_v2_update_pmap(&rh, p_map);
        int nvp = build_vp_list(p_map, tile_count, vp_tiles, 0.15f);
        printf("[PM] %d viewport tiles (threshold 0.15)\n", nvp);
 
        /* Step D: bandwidth estimate */
        float bw = harmonic_bw(bw_hist, BW_HIST);
        printf("[BW] %.2f Mbps\n", bw * 8.0f / 1e6f);
 
        /* Step E: CTS scheduling */
        float buf_pressure = (buf_level < B_HIGH)
                             ? (1.0f - buf_level / B_HIGH) : 0.0f;
        (void)buf_pressure;   /* used by RA-MPC internally */
 
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
 
        memset(&sched_out, 0, sizeof(sched_out));
        sched_out.tiles      = tiles;
        sched_out.tile_count = tile_count;
 
        if (cts_schedule(ACTIVE_ALGORITHM, &cin, &sched_out,
                         (ACTIVE_ALGORITHM == SCHEDULER_SLR) ? &slr_state : NULL)
            != RET_SUCCESS) {
            fprintf(stderr, "[main] cts_schedule failed\n");
            break;
        }
 
        printf("[%s] J=%.2f  BW=%.2f Mbps  CPU=%.3f  VP_avg=%.0f bps\n",
               algo_name, sched_out.objective_value,
               sched_out.total_bw_used * 8.0f / 1e6f,
               sched_out.total_cpu_load,
               sched_out.vp_avg_quality);
 
        if (ACTIVE_ALGORITHM == SCHEDULER_SLR)
            printf("[SLR] λ=%.3f  μ=%.3f  γ=%.3f\n",
                   slr_state.lambda, slr_state.mu, slr_state.gamma);
 
        if (nvp > 0) last_q = sched_out.tiles[vp_tiles[0]].chosen_version;
 
        /* Step F: parallel download with early termination */
        rh.cts_out = &sched_out;
        if (request_handler_v2_post_get_info(&rh, seg, vp_tiles, nvp,
                                             NULL,
                                             actual_yaw, actual_pitch,
                                             PROTOCOL, &rm) != RET_SUCCESS)
            fprintf(stderr, "[main] download seg %llu failed\n", seg+1);
 
        /* Step G: update bandwidth history */
        float tot_bytes = 0.0f, tot_us = 0.0f;
        for (int i = 0; i < nvp; i++) {
            int tid = vp_tiles[i];
            tot_bytes += (float)rh.size_dl[tid];
            tot_us    += (float)rh.total_time[tid];
        }
        float meas_bw = (tot_us > 0.0f) ? (tot_bytes / (tot_us / 1e6f)) : bw;
        bw_hist[bw_idx++ % BW_HIST] = meas_bw;
 
        /* Step H: simulate buffer */
        float dl_time = (bw > 0.0f) ? (sched_out.vp_avg_quality * nvp / bw) : 0.5f;
        buf_level = (buf_level >= dl_time) ? (buf_level - dl_time) : 0.0f;
        buf_level += SEGMENT_DURATION;
        if (buf_level > MAX_BUFFER_SIZE) buf_level = MAX_BUFFER_SIZE;
 
        /* Step I: feed HMD sample to predictor */
        add_viewport_sample(&vpes, actual_yaw, actual_pitch);
 
        /* Update state for next iteration */
        prev_pred_yaw = pred_yaw;
        prev_ts_ms    = actual_ts_ms;
 
        request_handler_v2_reset(&rh);
    }
 
    /* shutdown */
    printf("\n=== Session complete  Algorithm: %s ===\n", algo_name);
    printf("  Total early-terminated tiles : %d\n", rh.early_term_count);
    if (ACTIVE_ALGORITHM == SCHEDULER_SLR)
        printf("  Final SLR multipliers: λ=%.4f  μ=%.4f  γ=%.4f\n",
               slr_state.lambda, slr_state.mu, slr_state.gamma);
 
    free(tiles);
    request_handler_v2_destroy(&rh);
    return EXIT_SUCCESS;
}