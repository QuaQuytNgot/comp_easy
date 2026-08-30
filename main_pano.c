/*
 * Demonstrates the Pano 360-degree video scheduling algorithm.
 *
 * Pano Optimization Logic (SIGCOMM '19):
 * - Focuses strictly on maximizing perceived quality (augmented PSPNR) 
 * bounded by a strict network bandwidth constraint.
 * - Introduces 360JND (Just-Noticeable Difference) multipliers based on:
 * 1. Viewpoint moving speed (deg/s)
 * 2. Scene luminance change (Not simulated in this trace)
 * 3. Depth-of-Field difference (Not simulated in this trace)
 * - Bypasses CPU load metrics and early-termination recovery heuristics.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>       

#include "proto_comp/define.h"
#include "proto_comp/abr.h"
#include "proto_comp/viewport_prediction.h"
#include "proto_comp/request_handler_v2.h"
#include "proto_comp/cts_scheduler.h"
#include "proto_comp/metrics_logger.h"

/* ── configuration ──────────────────────────────────────────────────────── */
#define SERVER_ADDR              "https://192.168.101.17:8443"
#define TOTAL_SEGMENTS           10
#define VP_HISTORY_SZ            20
#ifndef PROTOCOL
#define PROTOCOL                 STREAM_HTTP_3_0
#endif
#define TAU_EARLY_TERM           0.00f  /* ĐÃ SỬA: Tắt ET từ lúc khởi tạo */
#define VP_HALF_YAW              (VIEWPORT_WIDTH_DEGREES  / 2.0f)
#define VP_HALF_PITCH            (VIEWPORT_HEIGHT_DEGREES / 2.0f)
#define SACCADE_THRESHOLD_DEG_S  30.0f
#define ACTIVE_ALGORITHM         SCHEDULER_MOSAIC

/* Bandwidth initialized in bits/s to match VIDEO_BIT_RATE[] */
#define BW_INIT_BITS_S           35000000.0f    /* 0.5 Mbps */

/* NEW CODE: Bộ lọc Viewport thực tế hiển thị trên màn hình (~6 Tiles) */
static int filter_actual_viewport_tiles(float yaw, float pitch, const int *predicted_vp, int n_pred, int *filtered_vp)
{
    float ny = wrap_angle_360(yaw);
    float np = clamp_pitch(pitch);
    int k = 0;

    for (int i = 0; i < n_pred; i++) {
        int tid = predicted_vp[i];
        int col = tid % NO_OF_COLS;
        int row = tid / NO_OF_COLS;
        
        float ty     = (col + 0.5f) * TILE_WIDTH;
        float tp     = -90.0f + (row + 0.5f) * TILE_HEIGHT;
        float dy_raw = fabsf(ny - ty);
        float dy     = (dy_raw > 180.0f) ? (360.0f - dy_raw) : dy_raw;
        float dp     = fabsf(np - tp);

        /* Chỉ giữ lại các tile thực sự nằm trong khung hình của kính VR */
        if (dy <= VP_HALF_YAW && dp <= VP_HALF_PITCH) {
            filtered_vp[k++] = tid;
        }
    }
    return k;
}

/* ── QoE constants ──────────────────────────────────────────────────────── */
#define MAX_TILE_BITRATE_KBPS    2000.0f
#define MAX_REBUFFER_S           5.0f
#define MAX_SMOOTHNESS_KBPS      1000.0f

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

/* ── Wall-clock timer ──────────────────────────────────────── */
static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ── main ───────────────────────────────────────────────────────────────── */
int main(void)
{
    const char *algo_name = "PANO";
    printf("=== CTS Streaming Demo  Algorithm: %s ===\n\n", algo_name);

    float total_qoe_score      = 0.0f;
    float prev_vp_bitrate_kbps = 0.0f;

    /* 1. Viewport predictor */
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

    /* 2. Request handler */
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

    /* 3. Scheduler structures */
    cts_tile_t *tiles = (cts_tile_t *)calloc(tile_count, sizeof(cts_tile_t));
    if (!tiles) { request_handler_v2_destroy(&rh); return EXIT_FAILURE; }

    cts_output_t sched_out = { .tiles = tiles, .tile_count = tile_count };

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
    const char *log_filename = "pano_metrics.csv";
    if (metrics_logger_init(&m_logger, log_filename) != 0)
        return EXIT_FAILURE;

    int chosen_bitrate_all[TOTAL_SEGMENTS];
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

        /* Step A: Viewport prediction */
        float pred_yaw = 180.0f, pred_pit = 0.0f;
        if (vpes.vpes_post(&vpes, &pred_yaw, &pred_pit) == RET_SUCCESS)
            printf("[VP] pred yaw=%.1f°  pitch=%.1f°\n", pred_yaw, pred_pit);
        else
            printf("[VP] prediction failed — using last centre\n");

        /* Calculate Viewport Velocity (Critical for Pano 360JND) */
        float dt_s      = (actual_ts_ms > prev_ts_ms)
                          ? (actual_ts_ms - prev_ts_ms) / 1000.0f : 1.0f;
        float d_yaw_raw = fabsf(pred_yaw - prev_pred_yaw);
        float d_yaw     = (d_yaw_raw > 180.0f) ? (360.0f - d_yaw_raw) : d_yaw_raw;
        float velocity  = d_yaw / dt_s;

        float adaptive_sigma = (velocity > 20.0f)
                               ? (VIEWPORT_WIDTH_DEGREES * 0.5f)
                               : (VIEWPORT_WIDTH_DEGREES * 0.3f);

        /* VÔ HIỆU HÓA EARLY TERMINATION (ET) ĐỂ ĐÁNH GIÁ CÔNG BẰNG VỚI MOSAIC */
        rh.pool->early_term_tau = 0.0f;
        
        printf("[TAU] velocity=%.1f°/s  sigma=%.1f°  tau=0.000 (Disabled)\n",
               velocity, adaptive_sigma);

        /* Step B: Probability map */
        build_pmap_adaptive(pred_yaw, pred_pit, p_map, tile_count, adaptive_sigma);
        request_handler_v2_update_pmap(&rh, p_map);
        int nvp = build_vp_list(p_map, tile_count, vp_tiles, 0.15f);
        printf("[PM] %d viewport tiles\n", nvp);

        /* Step C: Bandwidth estimate (bits/s) */
        float bw = harmonic_bw(bw_hist, BW_HIST) * 0.90f;
        printf("[BW] est %.2f Mbps\n", bw / 1e6f);

        /* Step D: CTS scheduling (PANO) */
        cts_input_t cin;
        cts_input_init(&cin);
        cin.p_map                 = p_map;
        cin.tile_count            = tile_count;
        cin.bandwidth             = bw;           /* bits/s */
        cin.buffer_level          = buf_level;
        cin.protocol              = PROTOCOL;
        cin.last_quality          = last_q;
        
        /* Pano-specific inputs */
        cin.viewpoint_speed_deg_s = velocity; 
        cin.tile_lum_change       = NULL; /* Missing from trace dataset */
        cin.tile_dof_diff         = NULL; /* Missing from trace dataset */

        memset(&sched_out, 0, sizeof(sched_out));
        sched_out.tiles      = tiles;
        sched_out.tile_count = tile_count;

        if (cts_schedule(ACTIVE_ALGORITHM, &cin, &sched_out, NULL) != RET_SUCCESS) {
            fprintf(stderr, "[main] cts_schedule failed\n");
            break;
        }
        
        cts_print_schedule(&sched_out, ACTIVE_ALGORITHM);

        printf("[%s] J=%.2f  BW=%.2f Mbps  VP_avg=%.0f bps\n",
               algo_name, sched_out.objective_value,
               sched_out.total_bw_used / 1e6f,
               sched_out.vp_avg_quality);

        if (nvp > 0) last_q = sched_out.tiles[vp_tiles[0]].chosen_version;
        
        chosen_bitrate_all[seg] = last_q;
        /* ── Step E: Download ────────────────────────────────────────────── */
        rh.cts_out = &sched_out;
        double t_start = now_seconds();

        /* Uses no-RM version to bypass stripped resource_monitor dependency */
        if (request_handler_v2_post_get_info_no_rm(&rh, seg, vp_tiles, nvp,
                                                   NULL, actual_yaw, actual_pitch,
                                                   PROTOCOL) != RET_SUCCESS) {
            fprintf(stderr, "[main] download seg %llu failed\n", seg+1);
        }

        double t_end = now_seconds();

        float actual_wall_dl_s = (float)(t_end - t_start);
        if (actual_wall_dl_s < 1e-4f) actual_wall_dl_s = 0.001f;  /* fallback */

        /* ── Step F: BW & Metrics ────────────────────────────────────────── */
        float tot_bytes = 0.0f;
        for (int i = 0; i < tile_count; i++) {
            tot_bytes += (float)rh.size_dl[i];
        }

        /* LỌC RA ĐÚNG ~6 TILE THỰC TẾ TRONG TẦM MẮT ĐỂ CHẤM ĐIỂM QOE */
        int playback_vp_tiles[NO_OF_ROWS * NO_OF_COLS];
        int n_playback_vp = filter_actual_viewport_tiles(actual_yaw, actual_pitch, vp_tiles, nvp, playback_vp_tiles);

        float current_vp_kbps_sum = 0.0f;
        for (int i = 0; i < n_playback_vp; i++) {
            int tid = playback_vp_tiles[i];
            current_vp_kbps_sum += ((float)rh.size_dl[tid] * 8.0f) / 1000.0f;
        }

        /* Chia trung bình dựa trên số lượng tile thực tế (n_playback_vp ≈ 6) */
        float current_avg_tile_kbps = (n_playback_vp > 0)
                                      ? (current_vp_kbps_sum / (float)n_playback_vp)
                                      : 0.0f;
        printf("nvp: %d\n", n_playback_vp);

        float meas_bw = (actual_wall_dl_s > 0.0f && tot_bytes > 0.0f)
                        ? (tot_bytes * 8.0f / actual_wall_dl_s)
                        : bw;
        bw_hist[bw_idx++ % BW_HIST] = meas_bw;

        float rebuffer_s = 0.0f;
        if (is_playing) {
            rebuffer_s = (actual_wall_dl_s > buf_level) ? (actual_wall_dl_s - buf_level) : 0.0f;
        }

        float smoothness_kbps = (seg == 0) ? 0.0f : fabsf(current_avg_tile_kbps - prev_vp_bitrate_kbps);

        float seg_qoe = calculate_qoe(current_avg_tile_kbps, rebuffer_s, smoothness_kbps);
        total_qoe_score += seg_qoe;

        /* IN THÔNG TIN ĐỐI CHỨNG */
        printf("[METRICS EVAL] Tiles in Viewport = %d (Đã đưa về chuẩn hình học)\n", n_playback_vp);
        printf("[METRICS] VP_BR=%.1f Kbps  Rebuf=%.3fs  Smooth=%.1f Kbps\n",
               current_avg_tile_kbps, rebuffer_s, smoothness_kbps);
        printf("[METRICS] Seg QoE=%.3f  Total QoE=%.3f\n", seg_qoe, total_qoe_score);

        prev_vp_bitrate_kbps = current_avg_tile_kbps;

        /* ── Step G: Buffer update ───────────────────────────────────────── */
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

        /* ── Step H: CSV Logging ─────────────────────────────────────────── */
        metrics_log_entry_t entry;
        entry.segment_id          = seg + 1;
        entry.network_bw_mbps     = meas_bw / 1e6f;
        entry.avg_vp_bitrate_kbps = current_avg_tile_kbps;
        entry.buffer_level_s      = buf_level; 
        entry.rebuffer_s          = rebuffer_s;
        entry.smoothness          = smoothness_kbps;
        
        if (!is_playing) {
            entry.seg_qoe = 0.0f;
        } else {
            entry.seg_qoe = seg_qoe;
        }
        
        entry.total_qoe  = total_qoe_score; 
        entry.et_count   = 0;    // Excluded: Only applies to SLR
        entry.cpu_rho    = 0.0f; // Excluded: Stripped CPU metric
        metrics_logger_write(&m_logger, &entry);


        /* Step I: Feed HMD sample */
        add_viewport_sample(&vpes, actual_yaw, actual_pitch);
        prev_pred_yaw = pred_yaw;
        prev_ts_ms    = actual_ts_ms;

        request_handler_v2_reset(&rh);
    }

    /* shutdown */
    printf("\n=== Session complete  Algorithm: %s ===\n", algo_name);
    printf("The chosen dl version is: ");
    for(int i = 0; i < TOTAL_SEGMENTS; i++)
    {
        printf("%d, ", chosen_bitrate_all[i]);
    }
    free(tiles);
    metrics_logger_close(&m_logger);
    printf("  Metrics → %s\n", log_filename);
    request_handler_v2_destroy(&rh);
    return EXIT_SUCCESS;
}
