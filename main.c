/*
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
#include <time.h>       /* clock_gettime — FIX BUG-2 */

#include "proto_comp/define.h"
#include "proto_comp/abr.h"
#include "proto_comp/viewport_prediction.h"
#include "proto_comp/request_handler_v2.h"
#include "proto_comp/cts_scheduler.h"
#include "proto_comp/resource_monitor.h"
#include "proto_comp/metrics_logger.h"

/* ── configuration ──────────────────────────────────────────────────────── */
#define SERVER_ADDR              "https://192.168.101.17:8443"
#define TOTAL_SEGMENTS           10
#define VP_HISTORY_SZ            20
#define PROTOCOL                 STREAM_HTTP_3_0
#define TAU_EARLY_TERM           0.10f
#define RHO_SYS_WARN             0.60f
#define VP_HALF_YAW              (VIEWPORT_WIDTH_DEGREES  / 2.0f)
#define VP_HALF_PITCH            (VIEWPORT_HEIGHT_DEGREES / 2.0f)
#define SACCADE_THRESHOLD_DEG_S  30.0f
#define ACTIVE_ALGORITHM         SCHEDULER_SLR

/* [BUG-1 FIX] Băng thông khởi tạo phải là bits/s để khớp VIDEO_BIT_RATE[] */
#define BW_INIT_BITS_S           500000.0f    /* 0.5 Mbps */

/* ── QoE constants ──────────────────────────────────────────────────────── */
#define MAX_TILE_BITRATE_KBPS    2000.0f
#define MAX_REBUFFER_S           5.0f
#define MAX_SMOOTHNESS_KBPS      1000.0f

/* [BUG-4 FIX] Signature mới: nhận rebuffer_s trực tiếp, không tự tính lại */
static float calculate_qoe(float avg_tile_bitrate_kbps,
                            float rebuffer_s,
                            float smoothness_kbps)
{
    float norm_bitrate  = avg_tile_bitrate_kbps / MAX_TILE_BITRATE_KBPS;
    float norm_rebuffer = rebuffer_s            / MAX_REBUFFER_S;
    float norm_smooth   = smoothness_kbps       / MAX_SMOOTHNESS_KBPS;

    if (norm_bitrate  > 1.0f) norm_bitrate  = 1.0f;
    if (norm_rebuffer > 1.0f) norm_rebuffer = 1.0f;
    if (norm_smooth   > 1.0f) norm_smooth   = 1.0f;

    return (QOE_ALPHA * norm_bitrate)
         - (QOE_BETA  * norm_rebuffer)
         - (QOE_GAMMA * norm_smooth);
}

/* ── viewport dataset ───────────────────────────────────────────────────── */
typedef struct { float yaw; float pitch; int timestamp; } ViewportSample;

ViewportSample viewport_dataset[] = {
    {180.0f,  0.0f,  0},
    {192.0f,  3.0f,  1000}, {198.0f,  9.0f, 2000},
    {177.0f, 12.0f,  3000}, {180.0f, 13.0f, 4000},
    {210.0f,  5.0f,  5000},
    {270.0f, 15.0f,  6000},
    {280.0f, 10.0f,  7000},
    {210.0f, 10.0f,  8000},
    {280.0f, 10.0f,  9000},
};

/* ── Top-Hat Gaussian probability map ──────────────────────────────────── */
static void build_pmap_adaptive(float yaw, float pitch,
                                float *p,  int   n,  float sigma)
{
    const float two_s2 = 2.0f * sigma * sigma;
    float ny = wrap_angle_360(yaw);
    float np = clamp_pitch(pitch);

    for (int i = 0; i < n; i++) {
        int   col    = i % NO_OF_COLS;
        int   row    = i / NO_OF_COLS;
        float ty     = (col + 0.5f) * TILE_WIDTH;
        float tp     = -90.0f + (row + 0.5f) * TILE_HEIGHT;
        float dy_raw = fabsf(ny - ty);
        float dy     = (dy_raw > 180.0f) ? (360.0f - dy_raw) : dy_raw;
        float dp     = fabsf(np - tp);

        if (dy <= VP_HALF_YAW && dp <= VP_HALF_PITCH) {
            p[i] = 1.0f;
        } else {
            float ey = (dy > VP_HALF_YAW)  ? (dy - VP_HALF_YAW)  : 0.0f;
            float ep = (dp > VP_HALF_PITCH) ? (dp - VP_HALF_PITCH) : 0.0f;
            p[i] = expf(-(ey * ey + ep * ep) / two_s2);
        }
    }
}

static int build_vp_list(const float *p, int n, int *vp, float thr)
{
    int k = 0;
    for (int i = 0; i < n; i++) if (p[i] >= thr) vp[k++] = i;
    return k;
}

/* ── harmonic bandwidth estimator (bits/s) ─────────────────────────────── */
#define BW_HIST 3
static float harmonic_bw(float *h, int n)
{
    float s = 0.0f; int c = 0;
    for (int i = 0; i < n; i++) if (h[i] > 0.0f) { s += 1.0f / h[i]; c++; }
    return (s > 0.0f && c > 0) ? ((float)c / s) : BW_INIT_BITS_S;
}

/* ── [BUG-2 FIX] Wall-clock timer ──────────────────────────────────────── */
static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ── main ───────────────────────────────────────────────────────────────── */
int main(void)
{
    const char *algo_name =
        (ACTIVE_ALGORITHM == SCHEDULER_RA_MPC) ? "RA-MPC" :
        (ACTIVE_ALGORITHM == SCHEDULER_SLR)    ? "SLR"    : "GREEDY";

    printf("=== CTS Streaming Demo  Algorithm: %s ===\n\n", algo_name);

    float total_qoe_score      = 0.0f;
    float prev_vp_bitrate_kbps = 0.0f;

    /* 1. Resource monitor */
    resource_monitor_t rm;
    if (resource_monitor_init(&rm) != RET_SUCCESS) {
        fprintf(stderr, "[main] resource_monitor_init failed\n");
        return EXIT_FAILURE;
    }
    printf("[INIT] Resource monitor OK\n");

    /* 2. Viewport predictor */
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
    int tile_count = NO_OF_ROWS * NO_OF_COLS;  /* 6 × 8 = 48 */
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
    cts_tile_t *tiles = (cts_tile_t *)calloc(tile_count, sizeof(cts_tile_t));
    if (!tiles) { request_handler_v2_destroy(&rh); return EXIT_FAILURE; }

    cts_output_t sched_out = { .tiles = tiles, .tile_count = tile_count };
    slr_state_t  slr_state;
    slr_state_init(&slr_state);

    float p_map[NO_OF_ROWS * NO_OF_COLS];
    int   vp_tiles[NO_OF_ROWS * NO_OF_COLS];

    float bw_hist[BW_HIST] = { BW_INIT_BITS_S, BW_INIT_BITS_S, BW_INIT_BITS_S };
    int   bw_idx    = 0;
    float buf_level = 0.0f;     
    bool  is_playing = false;   
    int   last_q    = 0;

    float prev_pred_yaw = 180.0f;
    int   prev_ts_ms    = 0;

    printf("[INIT] Ready.  Starting %d segments.\n\n", TOTAL_SEGMENTS);

    /* Metrics logger */
    metrics_logger_t m_logger;
    const char *log_filename = (ACTIVE_ALGORITHM == SCHEDULER_SLR)
                               ? "slr_metrics.csv" : "baseline_metrics.csv";
    if (metrics_logger_init(&m_logger, log_filename) != 0)
        return EXIT_FAILURE;

    /* ════════════════════════════════════════════════════════════════════
     * SEGMENT LOOP
     * ════════════════════════════════════════════════════════════════════ */
    for (COUNT seg = 0; seg < TOTAL_SEGMENTS; seg++) {
        printf("━━━━━━  SEG %llu  buf=%.2fs  ━━━━━━\n", seg+1, buf_level);

        int   data_idx     = (seg < (sizeof(viewport_dataset)/sizeof(ViewportSample)))
                             ? (int)seg : 0;
        float actual_yaw   = viewport_dataset[data_idx].yaw;
        float actual_pitch = viewport_dataset[data_idx].pitch;
        int   actual_ts_ms = viewport_dataset[data_idx].timestamp;

        /* Step A: resource */
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

        /* Adaptive sigma & tau */
        float dt_s      = (actual_ts_ms > prev_ts_ms)
                          ? (actual_ts_ms - prev_ts_ms) / 1000.0f : 1.0f;
        float d_yaw_raw = fabsf(pred_yaw - prev_pred_yaw);
        float d_yaw     = (d_yaw_raw > 180.0f) ? (360.0f - d_yaw_raw) : d_yaw_raw;
        float velocity  = d_yaw / dt_s;

        float adaptive_sigma = (velocity > 20.0f)
                               ? (VIEWPORT_WIDTH_DEGREES * 0.5f)
                               : (VIEWPORT_WIDTH_DEGREES * 0.3f);

        float diff_y_raw = fabsf(pred_yaw - actual_yaw);
        float diff_y     = (diff_y_raw > 180.0f) ? (360.0f - diff_y_raw) : diff_y_raw;
        float pred_err   = sqrtf(diff_y * diff_y +
                                 fabsf(pred_pit - actual_pitch) *
                                 fabsf(pred_pit - actual_pitch));

        float tau_base     = expf(-(pred_err * pred_err)
                                   / (2.0f * adaptive_sigma * adaptive_sigma));
        float tau_resource = 1.0f + 0.05f * slr_state.mu + 0.01f * slr_state.lambda;
        float tau_final    = tau_base * tau_resource;
        if (tau_final < 0.02f) tau_final = 0.02f;
        if (tau_final > 0.35f) tau_final = 0.35f;
        if (velocity > SACCADE_THRESHOLD_DEG_S) {
            tau_final = 0.0f;
            printf("[TAU] Saccade detected (%.1f°/s) — tau=0\n", velocity);
        }
        if (seg < 3) tau_final = 0.0f;

        rh.pool->early_term_tau = tau_final;
        printf("[TAU] velocity=%.1f°/s  sigma=%.1f°  tau=%.3f\n",
               velocity, adaptive_sigma, tau_final);

        /* Step C: probability map */
        build_pmap_adaptive(pred_yaw, pred_pit, p_map, tile_count, adaptive_sigma);
        request_handler_v2_update_pmap(&rh, p_map);
        int nvp = build_vp_list(p_map, tile_count, vp_tiles, 0.15f);
        printf("[PM] %d viewport tiles\n", nvp);

        /* Step D: bandwidth estimate (bits/s) */
        float bw = harmonic_bw(bw_hist, BW_HIST) * 0.90f;
        printf("[BW] est %.2f Mbps\n", bw / 1e6f);

        /* Step E: CTS scheduling */
        cts_input_t cin;
        cts_input_init(&cin);
        cin.p_map         = p_map;
        cin.tile_count    = tile_count;
        cin.bandwidth     = bw;           /* bits/s */
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
               sched_out.total_bw_used / 1e6f,
               sched_out.total_cpu_load, sched_out.vp_avg_quality);

        if (ACTIVE_ALGORITHM == SCHEDULER_SLR)
            printf("[SLR] λ=%.3f  μ=%.3f  γ=%.3f\n",
                   slr_state.lambda, slr_state.mu, slr_state.gamma);

        if (nvp > 0) last_q = sched_out.tiles[vp_tiles[0]].chosen_version;

        /* ── Step F: Download */
        rh.cts_out = &sched_out;

        double t_start = now_seconds();

        if (request_handler_v2_post_get_info(&rh, seg, vp_tiles, nvp,
                                             NULL, actual_yaw, actual_pitch,
                                             PROTOCOL, &rm) != RET_SUCCESS)
            fprintf(stderr, "[main] download seg %llu failed\n", seg+1);

        double t_end = now_seconds();

        float actual_wall_dl_s = (float)(t_end - t_start);
        if (actual_wall_dl_s < 1e-4f) actual_wall_dl_s = 0.001f;  /* fallback */

        /* ── Step G: BW & metrics ────────────────────────────*/
        float tot_bytes           = 0.0f;
        float current_vp_kbps_sum = 0.0f;

        for (int i = 0; i < tile_count; i++) {
            tot_bytes += (float)rh.size_dl[i];
        }

        for (int i = 0; i < nvp; i++) {
            int tid         = vp_tiles[i];
            current_vp_kbps_sum += ((float)rh.size_dl[tid] * 8.0f) / 1000.0f;
        }

        float current_avg_tile_kbps = (nvp > 0)
                                      ? (current_vp_kbps_sum / (float)nvp)
                                      : 0.0f;

        /* meas_bw = total bits / real wall-clock time (bits/s) */
        float meas_bw = (actual_wall_dl_s > 0.0f && tot_bytes > 0.0f)
                        ? (tot_bytes * 8.0f / actual_wall_dl_s)
                        : bw;
        bw_hist[bw_idx++ % BW_HIST] = meas_bw;
        printf("[BW] meas %.2f Mbps (%.3fs wall, %.1f KB)\n",
               meas_bw / 1e6f, actual_wall_dl_s, tot_bytes / 1024.0f);

        float rebuffer_s = 0.0f;
        if (is_playing) {
            rebuffer_s = (actual_wall_dl_s > buf_level)
                         ? (actual_wall_dl_s - buf_level)
                         : 0.0f;
        }

        float smoothness_kbps = (seg == 0)
                                ? 0.0f
                                : fabsf(current_avg_tile_kbps - prev_vp_bitrate_kbps);

        float seg_qoe = calculate_qoe(current_avg_tile_kbps,
                                      rebuffer_s,
                                      smoothness_kbps);
        total_qoe_score += seg_qoe;

        printf("[METRICS] VP_BR=%.1f Kbps  Rebuf=%.3fs  Smooth=%.1f Kbps\n",
               current_avg_tile_kbps, rebuffer_s, smoothness_kbps);
        printf("[METRICS] Seg QoE=%.3f  Total QoE=%.3f\n",
               seg_qoe, total_qoe_score);

        prev_vp_bitrate_kbps = current_avg_tile_kbps;

        /* ── Step H: Buffer update ───────────────────────────────────────── */
        /*Pre-buffering: first 3 segment will not count rebuffer*/
        if (!is_playing) {
            buf_level += SEGMENT_DURATION;
            printf("[PLAYER] Pre-buffering... (Buffer hiện tại: %.2fs)\n", buf_level);
            
            if (buf_level >= 3.0f) {  
                is_playing = true;
                printf("[PLAYER] Pre-buffering hoàn tất, video bắt đầu PLAY!\n");
            }
        } else {
            buf_level = (buf_level >= actual_wall_dl_s)
                        ? (buf_level - actual_wall_dl_s)
                        : 0.0f;
            buf_level += SEGMENT_DURATION;
        }

        if (buf_level > MAX_BUFFER_SIZE) buf_level = MAX_BUFFER_SIZE;
        printf("[BUF] %.2fs\n", buf_level);

        /* ========================================================
         * DI CHUYỂN KHỐI GHI LOG CSV XUỐNG ĐÂY (SAU KHI ĐÃ CỘNG BUFFER)
         * ======================================================== */
        metrics_log_entry_t entry;
        entry.segment_id          = seg + 1;
        entry.network_bw_mbps     = meas_bw / 1e6f;
        entry.avg_vp_bitrate_kbps = current_avg_tile_kbps;
        entry.buffer_level_s      = buf_level;  // Lúc này buf_level đã là 1.0s ở segment 1
        entry.rebuffer_s          = rebuffer_s;
        entry.smoothness          = smoothness_kbps;
        
        // Nhớ áp dụng luật QoE = 0 khi Pre-buffering giống như đã bàn ở lượt trước
        if (!is_playing) {
            entry.seg_qoe = 0.0f;
            // Lưu ý: Tùy logic của bạn có muốn trừ lại total_qoe_score đã cộng ở trên không
            // Ở đây mình chỉ set seg_qoe trong file CSV thành 0 để vẽ chart cho đúng
        } else {
            entry.seg_qoe = seg_qoe;
        }
        
        entry.total_qoe           = total_qoe_score; 
        entry.et_count   = (ACTIVE_ALGORITHM == SCHEDULER_SLR) ? rh.early_term_count : 0;
        entry.cpu_rho    = (ACTIVE_ALGORITHM == SCHEDULER_SLR) ? rho : 0.0f;
        metrics_logger_write(&m_logger, &entry);
        /* ======================================================== */

        /* Step I: feed HMD sample */
        add_viewport_sample(&vpes, actual_yaw, actual_pitch);
        prev_pred_yaw = pred_yaw;
        prev_ts_ms    = actual_ts_ms;

        request_handler_v2_reset(&rh);
    }

    /* shutdown */
    printf("\n=== Session complete  Algorithm: %s ===\n", algo_name);
    printf("  Total ET tiles     : %d\n", rh.early_term_count);
    if (ACTIVE_ALGORITHM == SCHEDULER_SLR)
        printf("  Final SLR: λ=%.4f  μ=%.4f  γ=%.4f\n",
               slr_state.lambda, slr_state.mu, slr_state.gamma);

    free(tiles);
    metrics_logger_close(&m_logger);
    printf("  Metrics → %s\n", log_filename);
    request_handler_v2_destroy(&rh);
    return EXIT_SUCCESS;
}