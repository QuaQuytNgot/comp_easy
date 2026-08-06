/*
 * main_BOLA.c  —  BOLA360 baseline for comparison with CTS/SLR
 *
 * Implements the BOLA360 per-tile buffer-based ABR algorithm from:
 *   "BOLA360: Near-Optimal Bitrate Adaptation for 360-Degree Video Streaming"
 *   arXiv:2309.04023v2
 *
 * Per-tile quality selection (Algorithm 2, Eq. 5a):
 *
 *   score(d, m) = [V*(v_m * p_{k,d} + γδ) - Q(t_k)/δ] / S_m
 *
 *   v_m = log(R_m / R_0 + 1)    logarithmic utility
 *   S_m = R_m * δ / 8           segment size (bytes) at bitrate m
 *   Q   = current buffer level (s)
 *   δ   = segment duration (1 s)
 *   V,γ = Lyapunov parameters
 *
 * Rule: m* = argmax_m score(d,m), but only among m with score > 0.
 *       If no m has positive score: select m=0 (minimum quality).
 *
 * V calibration (for R_max=756712, δ=1s, Q_max=10s):
 *   V=3.0 gives quality ladder: V0 at Q<1.3s → V4 at Q>4s → V0 at Q>7.2s.
 *   Non-VP tiles (p_d≈0) naturally get V0 since p_d term is near zero.
 *
 * This binary (no ET, no CPU-awareness) serves as the pure-BOLA baseline.
 * Compare against main_TA.c (SLR with ET + CPU awareness).
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
#include "proto_comp/resource_monitor.h"
#include "proto_comp/metrics_logger.h"
#include "proto_comp/metric_evaluator.h"

/* ── configuration ──────────────────────────────────────────────────────── */
#define SERVER_ADDR          "https://192.168.101.17:8443"
#define TOTAL_SEGMENTS       10
#define VP_HISTORY_SZ        20
#define PROTOCOL             STREAM_HTTP_2_0
#define RHO_SYS_WARN         0.80f
#define VP_HALF_YAW          (VIEWPORT_WIDTH_DEGREES  / 2.0f)
#define VP_HALF_PITCH        (VIEWPORT_HEIGHT_DEGREES / 2.0f)
#define SACCADE_THRESHOLD_DEG_S  30.0f
#define BW_INIT_BITS_S       35000000.0f   /* 35 Mbps initial estimate */

/* BOLA360 parameters */
#define BOLA_V               3.0f   /* Lyapunov parameter V */
#define BOLA_GAMMA           0.2f   /* utility floor scaling γ */
#define BOLA_DELTA           SEGMENT_DURATION  /* δ = 1s */
#define BOLA_NUM_Q           5      /* must match CTS_NUM_QUALITIES */

/* VP tile detection threshold */
#define BOLA_VP_THRESHOLD    0.15f

/* ── QoE constants ──────────────────────────────────────────────────────── */
#define MAX_TILE_BITRATE_KBPS  2000.0f
#define MAX_REBUFFER_S         5.0f
#define MAX_SMOOTHNESS_KBPS    1000.0f

static float calculate_qoe(float avg_tile_bitrate_kbps,
                            float rebuffer_s,
                            float smoothness_kbps)
{
    float norm_bitrate  = avg_tile_bitrate_kbps / MAX_TILE_BITRATE_KBPS;
    float norm_rebuffer = rebuffer_s             / MAX_REBUFFER_S;
    float norm_smooth   = smoothness_kbps        / MAX_SMOOTHNESS_KBPS;

    if (norm_bitrate  > 1.0f) norm_bitrate  = 1.0f;
    if (norm_rebuffer > 1.0f) norm_rebuffer = 1.0f;
    if (norm_smooth   > 1.0f) norm_smooth   = 1.0f;

    return (QOE_ALPHA * norm_bitrate)
         - (QOE_BETA  * norm_rebuffer)
         - (QOE_GAMMA * norm_smooth);
}

/* ── BOLA360 core ───────────────────────────────────────────────────────── */

/*
 * Bitrate table — must match VIDEO_BIT_RATE[] in abr.c.
 * Using float copies here to avoid double/float mixing.
 */
static const float BOLA_RATES[BOLA_NUM_Q] = {
    94712.0f, 184528.0f, 296800.0f, 475968.0f, 756712.0f
};

/*
 * bola_select_quality()
 *
 * Returns quality index m* ∈ [0, BOLA_NUM_Q) for one tile.
 *
 * @param p_d     Viewport probability of this tile (0..1)
 * @param buf_s   Current buffer level in seconds
 */
static int bola_select_quality(float p_d, float buf_s)
{
    int   best_m     = 0;
    float best_score = -1.0e30f;

    for (int m = 0; m < BOLA_NUM_Q; m++) {
        float R_m = BOLA_RATES[m];
        float v_m = logf(R_m / BOLA_RATES[0] + 1.0f);
        float S_m = R_m * BOLA_DELTA / 8.0f;

        float score = (BOLA_V * (v_m * p_d + BOLA_GAMMA * BOLA_DELTA)
                       - buf_s / BOLA_DELTA) / S_m;

        if (score > best_score) {
            best_score = score;
            best_m     = m;
        }
    }
    // Trả về đúng nghiệm của phương trình 5a (argmax)
    return best_m; 
}

/*
 * bola360_schedule()
 *
 * Fills out->tiles[d] for every tile using BOLA360 per-tile selection.
 * Does NOT enforce a global bandwidth budget — BOLA relies on buffer
 * dynamics to naturally regulate download rate over time.
 */
static void bola360_schedule(const float  *p_map,
                              int           tile_count,
                              float         buf_s,
                              cts_output_t *out)
{
    float total_bw   = 0.0f;
    float vp_bw_sum  = 0.0f;
    int   vp_count   = 0;

    for (int d = 0; d < tile_count; d++) {
        float p_d = p_map[d];
        int   m   = bola_select_quality(p_d, buf_s);

        out->tiles[d].tile_id          = d;
        out->tiles[d].probability      = p_d;
        out->tiles[d].chosen_version   = m;
        out->tiles[d].quality_bps      = BOLA_RATES[m];
        out->tiles[d].c_proc           = 0.0f;
        out->tiles[d].load_cpu         = 0.0f;
        out->tiles[d].net_utility      = 0.0f;
        out->tiles[d].early_terminated = 0;

        total_bw += BOLA_RATES[m];
        if (p_d >= BOLA_VP_THRESHOLD) {
            vp_bw_sum += BOLA_RATES[m];
            vp_count++;
        }
    }

    out->tile_count      = tile_count;
    out->total_bw_used   = total_bw;
    out->vp_avg_quality  = (vp_count > 0) ? (vp_bw_sum / (float)vp_count) : 0.0f;
    out->objective_value = buf_s;  /* expose buffer level as "objective" for logging */
}

/* ── viewport dataset ───────────────────────────────────────────────────── */
typedef struct { float yaw; float pitch; int timestamp; } ViewportSample;

static ViewportSample viewport_dataset[] = {
    {180.0f,  0.0f,  0},
    {192.0f,  3.0f,  1000}, {198.0f,  9.0f,  2000},
    {177.0f, 12.0f,  3000}, {180.0f, 13.0f,  4000},
    {210.0f,  5.0f,  5000},
    {270.0f, 15.0f,  6000},
    {280.0f, 10.0f,  7000},
    {210.0f, 10.0f,  8000},
    {280.0f, 10.0f,  9000},
};

/* ── probability map ────────────────────────────────────────────────────── */
static void build_pmap_adaptive(float yaw, float pitch,
                                 float *p,  int   n,  float sigma)
{
    const float two_s2 = 2.0f * sigma * sigma;
    float ny = wrap_angle_360(yaw);
    float np = clamp_pitch(pitch);

    float sum_p = 0.0f; // Khai báo biến để tính tổng xác suất

    // Bước 1: Tính xác suất thô và áp dụng mức sàn (floor)
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
        
        // --- PHẦN THÊM MỚI ---
        // Đảm bảo xác suất của non-VP tiles không bao giờ tụt về 0. 
        // Lấy mốc 0.017f dựa trên phân phối Heterogeneous của bài báo.
        if (p[i] < 0.017f) {
            p[i] = 0.017f; 
        }
        
        // Cộng dồn để lấy tổng xác suất
        sum_p += p[i]; 
    }

    // Bước 2: Chuẩn hóa xác suất (Normalization)
    // Đảm bảo tổng tất cả p_d của một chunk cộng lại bằng 1
    if (sum_p > 0.0f) {
        for (int i = 0; i < n; i++) {
            p[i] = p[i] / sum_p;
        }
    }
}

static int build_vp_list(const float *p, int n, int *vp, float thr)
{
    int k = 0;
    for (int i = 0; i < n; i++) if (p[i] >= thr) vp[k++] = i;
    return k;
}

/* ── harmonic bandwidth estimator ───────────────────────────────────────── */
#define BW_HIST 5
static float harmonic_bw(float *h, int n)
{
    float s = 0.0f; int c = 0;
    for (int i = 0; i < n; i++) if (h[i] > 0.0f) { s += 1.0f / h[i]; c++; }
    return (s > 0.0f && c > 0) ? ((float)c / s) : BW_INIT_BITS_S;
}

/* ── wall-clock timer ───────────────────────────────────────────────────── */
static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ── viewport grid visualizer ───────────────────────────────────────────── */
static void print_viewport_grid(const float *p_pred, float actual_yaw,
                                  float actual_pitch, float sigma)
{
    float p_act[NO_OF_ROWS * NO_OF_COLS];
    build_pmap_adaptive(actual_yaw, actual_pitch, p_act,
                        NO_OF_ROWS * NO_OF_COLS, sigma);

    const float thr = BOLA_VP_THRESHOLD;
    printf("[VP GRID]  B=both  P=pred_only  A=actual_only  .=outside\n");
    printf("        ");
    for (int c = 0; c < NO_OF_COLS; c++) printf(" C%d ", c);
    printf("\n");
    for (int r = 0; r < NO_OF_ROWS; r++) {
        printf("  R%d   ", r);
        for (int c = 0; c < NO_OF_COLS; c++) {
            int i  = r * NO_OF_COLS + c;
            int ip = p_pred[i] >= thr;
            int ia = p_act[i]  >= thr;
            char ch = (ip && ia) ? 'B' : ip ? 'P' : ia ? 'A' : '.';
            printf("[%c] ", ch);
        }
        printf("\n");
    }
}

/* ── quality grid ────────────────────────────────────────────────────────── */
static void print_quality_grid(const cts_output_t *sched,
                                const float        *p_map,
                                const count_time_t *size_dl)
{
    const float thr = BOLA_VP_THRESHOLD;
    printf("[QUAL GRID]  VP=[Vn]  non-VP=[vn]  (V0=lowest V4=highest)\n");
    printf("        ");
    for (int c = 0; c < NO_OF_COLS; c++) printf(" C%d  ", c);
    printf("\n");
    for (int r = 0; r < NO_OF_ROWS; r++) {
        printf("  R%d   ", r);
        for (int c = 0; c < NO_OF_COLS; c++) {
            int i = r * NO_OF_COLS + c;
            int v = sched->tiles[i].chosen_version;
            if (size_dl[i] == 0)
                printf("[??] ");
            else if (p_map[i] >= thr)
                printf("[V%d] ", v);
            else
                printf("[v%d] ", v);
        }
        printf("\n");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("=== BOLA360 Streaming  V=%.1f  γ=%.2f  δ=%.1fs ===\n\n",
           BOLA_V, BOLA_GAMMA, BOLA_DELTA);

    float total_qoe_score      = 0.0f;
    float prev_vp_bitrate_kbps = 0.0f;

    /* 1. Resource monitor (for CPU measurement, not used in BOLA decision) */
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
                  VIEWPORT_ESTIMATOR_KALMAN) != RET_SUCCESS) {
        fprintf(stderr, "[main] vpes_init failed\n");
        return EXIT_FAILURE;
    }
    printf("[INIT] Viewport predictor OK\n");

    /* 3. Request handler — no Early Termination (tau=0) for clean baseline */
    int tile_count = NO_OF_ROWS * NO_OF_COLS;
    request_handler_v2_t rh;
    if (request_handler_v2_init(&rh, SERVER_ADDR, TOTAL_SEGMENTS,
                                5, (COUNT)tile_count, PROTOCOL,
                                MAX_PARALLEL_VP_TILES) != RET_SUCCESS) {
        fprintf(stderr, "[main] rh init failed\n");
        return EXIT_FAILURE;
    }
    rh.pool->early_term_tau = 0.0f;  /* BOLA360 baseline: no ET */
    printf("[INIT] Request handler OK (tau=0, no ET)\n");

    /* 4. Scheduler structures */
    cts_tile_t *tiles = (cts_tile_t *)calloc(tile_count, sizeof(cts_tile_t));
    if (!tiles) { request_handler_v2_destroy(&rh); return EXIT_FAILURE; }

    cts_output_t sched_out = { .tiles = tiles, .tile_count = tile_count };

    float p_map[NO_OF_ROWS * NO_OF_COLS];
    int   vp_tiles[NO_OF_ROWS * NO_OF_COLS];

    float bw_hist[BW_HIST] = { BW_INIT_BITS_S, BW_INIT_BITS_S, BW_INIT_BITS_S,
                               BW_INIT_BITS_S, BW_INIT_BITS_S };
    int   bw_idx    = 0;
    float buf_level = 0.0f;
    bool  is_playing = false;

    float prev_pred_yaw = 180.0f;
    int   prev_ts_ms    = 0;

    printf("[INIT] Ready.  Starting %d segments.\n\n", TOTAL_SEGMENTS);

    /* Metrics logger */
    metrics_logger_t m_logger;
    if (metrics_logger_init(&m_logger, "bola_metrics.csv") != 0)
        return EXIT_FAILURE;

    metric_tracker_t my_metrics;
    metric_tracker_init(&my_metrics);

    /* ════════════════════════════════════════════════════════════════════
     * SEGMENT LOOP
     * ════════════════════════════════════════════════════════════════════ */
    for (COUNT seg = 0; seg < TOTAL_SEGMENTS; seg++) {
        printf("━━━━━━  SEG %llu  buf=%.2fs  BOLA_Q/δ=%.2f  ━━━━━━\n",
               seg+1, buf_level, buf_level / BOLA_DELTA);

        int   data_idx     = (seg < (sizeof(viewport_dataset)/sizeof(ViewportSample)))
                             ? (int)seg : 0;
        float actual_yaw   = viewport_dataset[data_idx].yaw;
        float actual_pitch = viewport_dataset[data_idx].pitch;
        int   actual_ts_ms = viewport_dataset[data_idx].timestamp;

        /* Step A: resource monitor (measurement only, not used in scheduling) */
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

        /* Adaptive sigma */
        float dt_s      = (actual_ts_ms > prev_ts_ms)
                          ? (actual_ts_ms - prev_ts_ms) / 1000.0f : 1.0f;
        float d_yaw_raw = fabsf(pred_yaw - prev_pred_yaw);
        float d_yaw     = (d_yaw_raw > 180.0f) ? (360.0f - d_yaw_raw) : d_yaw_raw;
        float velocity  = d_yaw / dt_s;

        float adaptive_sigma = (velocity > 20.0f)
                               ? (VIEWPORT_WIDTH_DEGREES * 0.5f)
                               : (VIEWPORT_WIDTH_DEGREES * 0.3f);

        /* Prediction error */
        float diff_y_raw = fabsf(pred_yaw - actual_yaw);
        float diff_y     = (diff_y_raw > 180.0f) ? (360.0f - diff_y_raw) : diff_y_raw;
        float pred_err   = sqrtf(diff_y * diff_y +
                                  fabsf(pred_pit - actual_pitch) *
                                  fabsf(pred_pit - actual_pitch));
        printf("[VP] pred=(%.1f, %.1f)  actual=(%.1f, %.1f)  err=%.2f deg\n",
               pred_yaw, pred_pit, actual_yaw, actual_pitch, pred_err);

        /* Step C: probability map */
        build_pmap_adaptive(pred_yaw, pred_pit, p_map, tile_count, adaptive_sigma);
        request_handler_v2_update_pmap(&rh, p_map);
        int nvp = build_vp_list(p_map, tile_count, vp_tiles, BOLA_VP_THRESHOLD);
        printf("[PM] %d viewport tiles\n", nvp);
        print_viewport_grid(p_map, actual_yaw, actual_pitch, adaptive_sigma);

        /* Viewport prediction accuracy */
        float p_act_local[NO_OF_ROWS * NO_OF_COLS];
        build_pmap_adaptive(actual_yaw, actual_pitch, p_act_local, tile_count, adaptive_sigma);
        int tp = 0, fp = 0, fn = 0;
        for (int i = 0; i < tile_count; i++) {
            int pred_vp = (p_map[i]       >= BOLA_VP_THRESHOLD);
            int act_vp  = (p_act_local[i] >= BOLA_VP_THRESHOLD);
            if  (pred_vp && act_vp)       tp++;
            else if (pred_vp && !act_vp)  fp++;
            else if (!pred_vp && act_vp)  fn++;
        }
        float recall = (tp + fn > 0) ? (float)tp / (float)(tp + fn) : 1.0f;
        float iou    = (tp + fp + fn > 0) ? (float)tp / (float)(tp + fp + fn) : 1.0f;
        printf("[PRED] err=%.2f°  Recall=%.3f  IoU=%.3f  (TP=%d FP=%d FN=%d)\n",
               pred_err, recall, iou, tp, fp, fn);

        /* Step D: bandwidth estimate (bits/s) */
        float bw_avg  = harmonic_bw(bw_hist, BW_HIST);
        float bw_last = bw_hist[(bw_idx - 1 + BW_HIST) % BW_HIST];
        float bw      = fminf(bw_avg, bw_last * 1.1f);
        printf("[BW] est %.2f Mbps (avg=%.2f last=%.2f)\n",
               bw / 1e6f, bw_avg / 1e6f, bw_last / 1e6f);

        /* Step E: BOLA360 scheduling */
        memset(&sched_out, 0, sizeof(sched_out));
        sched_out.tiles      = tiles;
        sched_out.tile_count = tile_count;

        bola360_schedule(p_map, tile_count, buf_level, &sched_out);

        printf("[BOLA] Q=%.2fs  VP_avg=%.0f bps  total_bw=%.2f Mbps\n",
               buf_level, sched_out.vp_avg_quality,
               sched_out.total_bw_used / 1e6f);

        /* Print selected qualities for VP tiles */
        printf("[VP QUALITIES] ");
        for (int i = 0; i < nvp; i++) {
            int tid = vp_tiles[i];
            printf("T%d:V%d ", tid, sched_out.tiles[tid].chosen_version);
        }
        printf("\n");

        /* ── Step F: Download */
        rh.cts_out = &sched_out;

        double t_start = now_seconds();

        if (request_handler_v2_post_get_info(&rh, seg, vp_tiles, nvp,
                                              NULL, actual_yaw, actual_pitch,
                                              PROTOCOL, &rm) != RET_SUCCESS)
            fprintf(stderr, "[main] download seg %llu failed\n", seg+1);

        double t_end = now_seconds();

        print_quality_grid(&sched_out, p_map, rh.size_dl);

        float actual_wall_dl_s = (float)(t_end - t_start);
        if (actual_wall_dl_s < 1e-4f) actual_wall_dl_s = 0.001f;

        /* ── Step G: BW & metrics ─────────────────────────────────────── */
        float tot_bytes           = 0.0f;
        float current_vp_kbps_sum = 0.0f;

        for (int i = 0; i < tile_count; i++)
            tot_bytes += (float)rh.size_dl[i];

        for (int i = 0; i < nvp; i++) {
            int tid = vp_tiles[i];
            current_vp_kbps_sum += ((float)rh.size_dl[tid] * 8.0f) / 1000.0f;
        }

        float current_avg_tile_kbps = (nvp > 0)
                                       ? (current_vp_kbps_sum / (float)nvp)
                                       : 0.0f;

        /* bytes/s → bits/s */
        float meas_bw = (actual_wall_dl_s > 0.0f && tot_bytes > 0.0f)
                         ? (tot_bytes / actual_wall_dl_s)
                         : bw / 8.0f;
        bw_hist[bw_idx++ % BW_HIST] = meas_bw * 8.0f;
        printf("[BW] meas %.2f Mbps (%.3fs wall, %.1f KB)\n",
               meas_bw * 8.0f / 1e6f, actual_wall_dl_s, tot_bytes / 1024.0f);

        float rebuffer_s = 0.0f;
        if (is_playing) {
            rebuffer_s = (actual_wall_dl_s > buf_level)
                          ? (actual_wall_dl_s - buf_level)
                          : 0.0f;
        }

        float smoothness_kbps = (seg == 0)
                                  ? 0.0f
                                  : fabsf(current_avg_tile_kbps - prev_vp_bitrate_kbps);

        float seg_qoe = calculate_qoe(current_avg_tile_kbps, rebuffer_s, smoothness_kbps);
        total_qoe_score += seg_qoe;

        printf("[METRICS] VP_BR=%.1f Kbps  Rebuf=%.3fs  Smooth=%.1f Kbps\n",
               current_avg_tile_kbps, rebuffer_s, smoothness_kbps);
        printf("[METRICS] Seg QoE=%.3f  Total QoE=%.3f\n",
               seg_qoe, total_qoe_score);

        prev_vp_bitrate_kbps = current_avg_tile_kbps;

        /* ── Step H: Buffer update ──────────────────────────────────────── */
        if (!is_playing) {
            buf_level += SEGMENT_DURATION;
            printf("[PLAYER] Pre-buffering... (buf=%.2fs)\n", buf_level);
            if (buf_level >= 4.0f) {
                is_playing = true;
                printf("[PLAYER] Pre-buffering complete, video PLAYING!\n");
            }
        } else {
            buf_level = (buf_level >= actual_wall_dl_s)
                         ? (buf_level - actual_wall_dl_s)
                         : 0.0f;
            buf_level += SEGMENT_DURATION;
        }
        if (buf_level > MAX_BUFFER_SIZE) buf_level = MAX_BUFFER_SIZE;
        printf("[BUF] %.2fs\n", buf_level);

        /* ── Log entry ──────────────────────────────────────────────────── */
        metrics_log_entry_t entry;
        entry.segment_id          = seg + 1;
        entry.network_bw_mbps     = meas_bw * 8.0f / 1e6f;
        entry.avg_vp_bitrate_kbps = current_avg_tile_kbps;
        entry.buffer_level_s      = buf_level;
        entry.rebuffer_s          = rebuffer_s;
        entry.smoothness          = smoothness_kbps;
        entry.et_count            = 0;   /* no ET in BOLA baseline */
        entry.cpu_rho             = rho;
        entry.pred_err_deg        = pred_err;
        entry.vp_recall           = recall;
        entry.vp_iou              = iou;

        if (!is_playing) {
            entry.seg_qoe = 0.0f;
            total_qoe_score -= seg_qoe;
        } else {
            entry.seg_qoe = seg_qoe;
        }
        entry.total_qoe = total_qoe_score;

        metrics_logger_write(&m_logger, &entry);

        /* ── Step I: feed HMD sample ─────────────────────────────────────*/
        add_viewport_sample(&vpes, actual_yaw, actual_pitch);
        prev_pred_yaw = pred_yaw;
        prev_ts_ms    = actual_ts_ms;

        request_handler_v2_reset(&rh);
    }

    /* shutdown */
    printf("\n=== Session complete  BOLA360 ===\n");
    print_evaluation_report(&my_metrics, "BOLA360 Baseline");

    free(tiles);
    metrics_logger_close(&m_logger);
    printf("  Metrics → bola_metrics.csv\n");
    request_handler_v2_destroy(&rh);
    return EXIT_SUCCESS;
}
