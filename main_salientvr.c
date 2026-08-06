/*
 * Demonstrates the SalientVR 360-degree video scheduling algorithm.
 *
 * SalientVR Optimization Logic (MobiCom '22):
 * - Focuses on precise gaze-driven saliency rather than viewport bounds.
 * - Enforces network constraints as strict capacity boundaries to avoid 
 * Lagrangian tuning instabilities.
 * - Solves allocation mathematically via Simulated Annealing with a Monotonic constraint.
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
#include "proto_comp/bw_estimator.h"

/* ── configuration ──────────────────────────────────────────────────────── */
#define SERVER_ADDR              "https://192.168.101.17:8443"
#define TOTAL_SEGMENTS           10
#define VP_HISTORY_SZ            20
#define PROTOCOL                 STREAM_HTTP_2_0
#define ACTIVE_ALGORITHM         SCHEDULER_SALIENTVR
#define BW_INIT_BITS_S           35000000.0f    /* 0.5 Mbps */
#define SVR_EPSILON              0.3f         /* Low-salient weight */

/* Half-extents of the 90×90° viewport */
#define VP_HALF_YAW              (VIEWPORT_WIDTH_DEGREES  / 2.0f)
#define VP_HALF_PITCH            (VIEWPORT_HEIGHT_DEGREES / 2.0f)

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

typedef struct { float yaw; float pitch; int timestamp; } ViewportSample;

ViewportSample viewport_dataset[] = {
    {180.0f,  0.0f,  0},
    {192.0f,  3.0f,  1000}, {198.0f,  9.0f, 2000},
    {177.0f, 12.0f,  3000}, {180.0f, 13.0f, 4000},
    {210.0f,  5.0f,  5000}, {270.0f, 15.0f,  6000},
    {280.0f, 10.0f,  7000}, {210.0f, 10.0f,  8000},
    {280.0f, 10.0f,  9000},
};

/* ── SalientVR Gaze-Driven Saliency Judger (Section 4.1) ───────────────── */
static void build_salientvr_pmap(float head_yaw, float head_pitch,
                                 float gaze_yaw, float gaze_pitch,
                                 float *p, int n)
{
    for (int i = 0; i < n; i++) {
        int col = i % NO_OF_COLS;
        int row = i / NO_OF_COLS;
        float ty = (col + 0.5f) * TILE_WIDTH;
        float tp = -90.0f + (row + 0.5f) * TILE_HEIGHT;
        
        /* Distance to simulated gaze point */
        float dy_gaze = fabsf(wrap_angle_360(gaze_yaw) - ty);
        if (dy_gaze > 180.0f) dy_gaze = 360.0f - dy_gaze;
        float dp_gaze = fabsf(clamp_pitch(gaze_pitch) - tp);
        float dist_gaze = sqrtf(dy_gaze*dy_gaze + dp_gaze*dp_gaze);
        
        /* Distance to head center (Viewport) */
        float dy_head = fabsf(wrap_angle_360(head_yaw) - ty);
        if (dy_head > 180.0f) dy_head = 360.0f - dy_head;
        float dp_head = fabsf(clamp_pitch(head_pitch) - tp);
        
        /* Figure 7: Saliency Judgment Rules */
        if (dist_gaze <= 25.0f) {
            p[i] = 1.0f;          // High-salient gaze region
        } else if (dy_head <= VP_HALF_YAW && dp_head <= VP_HALF_PITCH) {
            p[i] = SVR_EPSILON;   // Low-salient hollow viewport
        } else {
            p[i] = 0.05f;         // FIX: Sửa 0.0f thành 0.05f để NVP không luôn là version 0
        }
    }
}

static int build_vp_list(const float *p, int n, int *vp, float thr) {
    int k = 0;
    for (int i = 0; i < n; i++) if (p[i] >= thr) vp[k++] = i;
    return k;
}

#define BW_HIST 3
static float harmonic_bw(float *h, int n) {
    float s = 0.0f; int c = 0;
    for (int i = 0; i < n; i++) if (h[i] > 0.0f) { s += 1.0f / h[i]; c++; }
    return (s > 0.0f && c > 0) ? ((float)c / s) : BW_INIT_BITS_S;
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void get_actual_vp_tiles(float yaw, float pitch, int tile_count, 
                                int *actual_vp_tiles, int *num_actual_vp)
{
    float ny = wrap_angle_360(yaw);
    float np = clamp_pitch(pitch);
    *num_actual_vp = 0;

    for (int i = 0; i < tile_count; i++) {
        int   col    = i % NO_OF_COLS;
        int   row    = i / NO_OF_COLS;
        float ty     = (col + 0.5f) * TILE_WIDTH;
        float tp     = -90.0f + (row + 0.5f) * TILE_HEIGHT;
        
        float dy_raw = fabsf(ny - ty);
        float dy     = (dy_raw > 180.0f) ? (360.0f - dy_raw) : dy_raw;
        float dp     = fabsf(np - tp);

        if (dy <= VP_HALF_YAW && dp <= VP_HALF_PITCH) {
            actual_vp_tiles[(*num_actual_vp)++] = i;
        }
    }
}

/* ── main ───────────────────────────────────────────────────────────────── */
int main(void)
{
    const char *algo_name = "SalientVR";
    printf("=== CTS Streaming Demo  Algorithm: %s ===\n\n", algo_name);

    srand((unsigned int)time(NULL));

    float total_qoe_score      = 0.0f;
    float prev_vp_bitrate_kbps = 0.0f;

    /* 1. Viewport predictor */
    float yaw_h[VP_HISTORY_SZ], pit_h[VP_HISTORY_SZ];
    int   ts[VP_HISTORY_SZ];
    for (int i = 0; i < VP_HISTORY_SZ; i++) {
        yaw_h[i] = 180.0f; pit_h[i] = 0.0f; ts[i] = i;
    }
    viewport_prediction_t vpes;
    vpes_init(&vpes, yaw_h, pit_h, ts, 0, VP_HISTORY_SZ, VP_HISTORY_SZ, VP_HISTORY_SZ, VIEWPORT_ESTIMATOR_LEGR);

    /* 2. Request handler */
    int tile_count = NO_OF_ROWS * NO_OF_COLS; 
    request_handler_v2_t rh;
    request_handler_v2_init(&rh, SERVER_ADDR, TOTAL_SEGMENTS, 5, (COUNT)tile_count, PROTOCOL, MAX_PARALLEL_VP_TILES);
    rh.pool->early_term_tau = 0.0f; // Disabled for SalientVR

    cts_tile_t *tiles = (cts_tile_t *)calloc(tile_count, sizeof(cts_tile_t));
    cts_output_t sched_out = { .tiles = tiles, .tile_count = tile_count };

    float p_map[NO_OF_ROWS * NO_OF_COLS];
    int   vp_tiles[NO_OF_ROWS * NO_OF_COLS];
    float bw_hist[BW_HIST] = { BW_INIT_BITS_S, BW_INIT_BITS_S, BW_INIT_BITS_S };
    int   bw_idx    = 0;
    float buf_level = 0.0f;     
    bool  is_playing = false;   
    int   last_q    = 0;

    /* Metrics logger */
    metrics_logger_t m_logger;
    const char *log_filename = "salientvr_metrics.csv";
    if (metrics_logger_init(&m_logger, log_filename) != 0)
        return EXIT_FAILURE;

    for (COUNT seg = 0; seg < TOTAL_SEGMENTS; seg++) {
        printf("━━━━━━  SEG %llu  buf=%.2fs  ━━━━━━\n", seg+1, buf_level);

        int   data_idx     = (seg < (sizeof(viewport_dataset)/sizeof(ViewportSample))) ? (int)seg : 0;
        float actual_yaw   = viewport_dataset[data_idx].yaw;
        float actual_pitch = viewport_dataset[data_idx].pitch;
        int   actual_ts_ms = viewport_dataset[data_idx].timestamp;

        /* Step A: Viewport prediction */
        float pred_yaw = 180.0f, pred_pit = 0.0f;
        vpes.vpes_post(&vpes, &pred_yaw, &pred_pit);

        /* Step B: Use Gaussian Heatmap to mimic SalientVR's DNN */
        float eval_p_map[NO_OF_ROWS * NO_OF_COLS];
        float adaptive_sigma = 27.0f; 
        build_salientvr_pmap(pred_yaw, pred_pit, pred_yaw, pred_pit, eval_p_map, tile_count);
        
        /* Generate the 24-tile list for the HTTP downloader and QoE metric */
        request_handler_v2_update_pmap(&rh, eval_p_map);
        int nvp = 0;
        get_actual_vp_tiles(pred_yaw, pred_pit, tile_count, vp_tiles, &nvp);

        /* Step C: Bandwidth estimate */
        float bw = harmonic_bw(bw_hist, bw_idx) * 0.90f;

        /* Step D: CTS scheduling (SalientVR) */
        cts_input_t cin;
        cts_input_init(&cin);
        
        /* THE FIX: Feed the continuous heatmap directly to SalientVR */
        cin.p_map        = eval_p_map; 
        cin.tile_count   = tile_count;
        cin.bandwidth    = bw;           
        cin.buffer_level = buf_level;
        cin.last_quality = last_q;
        
        cts_schedule(ACTIVE_ALGORITHM, &cin, &sched_out, NULL);
        
        // Track average chunk quality to feed DC_k temporal smoothness in next loop
        float chunk_avg_q = 0;
        for(int i=0; i<tile_count; i++) chunk_avg_q += sched_out.tiles[i].chosen_version;
        last_q = (int)roundf(chunk_avg_q / tile_count);

        /* ==================== THÊM ĐOẠN LOG VÀO ĐÂY ==================== */
        printf("[VP QUALITIES] ");
        for (int i = 0; i < nvp; i++) {
            int tid = vp_tiles[i];
            int version = sched_out.tiles[tid].chosen_version;
            
            // Chỉ in Tile ID và Version, gỡ bỏ hoàn toàn Kbps
            printf("T%d:V%d ", tid, version);
        }
        printf("\n");

        /* Log thêm các tile ngoài Viewport (NVP) để kiểm tra việc phân bổ chất lượng */
        printf("[NVP QUALITIES (V > 0)] ");
        int nvp_upgraded = 0;
        for (int i = 0; i < tile_count; i++) {
            // Kiểm tra xem tile i có nằm trong vp_tiles không
            int is_vp = 0;
            for (int j = 0; j < nvp; j++) {
                if (vp_tiles[j] == i) { is_vp = 1; break; }
            }
            
            // Nếu là NVP và thuật toán cấp chất lượng > 0 thì in ra
            if (!is_vp) {
                int version = sched_out.tiles[i].chosen_version;
                if (version > 0) { 
                    printf("T%d:V%d ", i, version);
                    nvp_upgraded++;
                }
            }
        }
        if (nvp_upgraded == 0) printf("None");
        printf("\n");
        /* =============================================================== */

        /* Step E: Download */
        rh.cts_out = &sched_out;
        double t_start = now_seconds();
        request_handler_v2_post_get_info_no_rm(&rh, seg, vp_tiles, nvp, NULL, actual_yaw, actual_pitch, PROTOCOL);
        double t_end = now_seconds();

        /* ── Step F: BW Metrics & Buffer Management ──────────────────────── */
        float actual_wall_dl_s = (float)(t_end - t_start);
        if (actual_wall_dl_s < 1e-4f) actual_wall_dl_s = 0.001f;

        float tot_bytes = 0.0f;
        for (int i = 0; i < tile_count; i++) {
            tot_bytes += (float)rh.size_dl[i];
        }
        
        /* THE FIX: CHỈ TÍNH QoE TRONG ACTUAL VIEWPORT (90x90 độ) */
        int strict_nvp = 0;
        float strict_vp_kbps_sum = 0.0f;
        
        for (int i = 0; i < tile_count; i++) {
            int col = i % NO_OF_COLS;
            int row = i / NO_OF_COLS;
            float ty = (col + 0.5f) * TILE_WIDTH;
            float tp = -90.0f + (row + 0.5f) * TILE_HEIGHT;
            
            float dy_raw = fabsf(wrap_angle_360(actual_yaw) - ty);
            float dy = (dy_raw > 180.0f) ? (360.0f - dy_raw) : dy_raw;
            float dp = fabsf(clamp_pitch(actual_pitch) - tp);
            
            /* Nếu tile nằm gọn trong 90x90 độ (Mỗi chiều lệch tối đa 45 độ) */
            if (dy <= VP_HALF_YAW && dp <= VP_HALF_PITCH) {
                strict_nvp++;
                strict_vp_kbps_sum += ((float)rh.size_dl[i] * 8.0f) / 1000.0f;
            }
        }
        printf("nvp: %d\n", strict_nvp);

        /* Trung bình bitrate CHỈ CỦA CÁC TILE NGƯỜI DÙNG THỰC SỰ NHÌN THẤY */
        float current_avg_tile_kbps = (strict_nvp > 0) ? (strict_vp_kbps_sum / (float)strict_nvp) : 0.0f;
        
        /* Xử lý lỗi đo băng thông khi dung lượng chunk quá nhỏ (Tránh Spiral) */
        float meas_bw = (actual_wall_dl_s > 0.0f && tot_bytes > 0.0f) ? (tot_bytes * 8.0f / actual_wall_dl_s) : bw;
        if (tot_bytes < 3000000.0f && meas_bw < bw) {
            meas_bw = bw; 
        }
        bw_hist[bw_idx++ % BW_HIST] = meas_bw;
        
        float rebuffer_s = 0.0f;
        if (is_playing) {
            rebuffer_s = (actual_wall_dl_s > buf_level) ? (actual_wall_dl_s - buf_level) : 0.0f;
        }
        
        float smoothness_kbps = (seg == 0) ? 0.0f : fabsf(current_avg_tile_kbps - prev_vp_bitrate_kbps);
        float seg_qoe = calculate_qoe(current_avg_tile_kbps, rebuffer_s, smoothness_kbps);
        total_qoe_score += seg_qoe;
        
        printf("[METRICS] VP_BR=%.1f Kbps  Rebuf=%.3fs  Smooth=%.1f Kbps\n", current_avg_tile_kbps, rebuffer_s, smoothness_kbps);
        printf("[METRICS] Seg QoE=%.3f  Total QoE=%.3f\n", seg_qoe, total_qoe_score);
        
        prev_vp_bitrate_kbps = current_avg_tile_kbps;
        
        if (!is_playing) {
            buf_level += SEGMENT_DURATION;
            printf("[PLAYER] Pre-buffering... (Buffer hiện tại: %.2fs)\n", buf_level);
            if (buf_level >= 3.0f) {
                is_playing = true;
                printf("[PLAYER] Pre-buffering hoàn tất, video bắt đầu PLAY!\n");
            }
        } else {
            buf_level = (buf_level >= actual_wall_dl_s) ? (buf_level - actual_wall_dl_s) : 0.0f;
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
        entry.et_count   = 0;
        entry.cpu_rho    = 0.0f; 
        metrics_logger_write(&m_logger, &entry);

        /* Step I: feed HMD sample */
        add_viewport_sample(&vpes, actual_yaw, actual_pitch);
        request_handler_v2_reset(&rh);
    }

    /* shutdown */
    printf("\n=== Session complete  Algorithm: %s ===\n", algo_name);

    free(tiles);
    metrics_logger_close(&m_logger);
    printf("  Metrics → %s\n", log_filename);
    request_handler_v2_destroy(&rh);
    return EXIT_SUCCESS;
}