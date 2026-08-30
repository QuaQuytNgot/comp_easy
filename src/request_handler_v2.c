#include "proto_comp/request_handler_v2.h"
#include "proto_comp/viewport_prediction.h" /* wrap_angle_360, clamp_pitch */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "proto_comp/http_pool.h"
#include "proto_comp/resource_monitor.h"

/* Half-extents of the 90×90° viewport */
#ifndef VP_HALF_YAW
#define VP_HALF_YAW (VIEWPORT_WIDTH_DEGREES / 2.0f) /* 45° */
#endif
#ifndef VP_HALF_PITCH
#define VP_HALF_PITCH (VIEWPORT_HEIGHT_DEGREES / 2.0f) /* 45° */
#endif

/* The build selects a dataset, so each binary requests tile names that match
 * the video it was built to evaluate. */
#define VIDEO_DATASET_CUTROPE 1
#define VIDEO_DATASET_DIVING  2
#define VIDEO_DATASET_UFO     3

#ifndef VIDEO_DATASET
#define VIDEO_DATASET VIDEO_DATASET_CUTROPE
#endif

#if VIDEO_DATASET == VIDEO_DATASET_CUTROPE
#define VIDEO_PATH_PREFIX "Class2_CutRope/Class2_CutRope"
#define VIDEO_TILE_RESOLUTION "480x341.333333333333"
#elif VIDEO_DATASET == VIDEO_DATASET_DIVING
#define VIDEO_PATH_PREFIX "Class1_Diving/Class1_Diving"
#define VIDEO_TILE_RESOLUTION "480x320"
#elif VIDEO_DATASET == VIDEO_DATASET_UFO
#define VIDEO_PATH_PREFIX "Class2_UFO/Class2_UFO"
#define VIDEO_TILE_RESOLUTION "480x320"
#else
#error "Unsupported VIDEO_DATASET"
#endif

/* CURLINFO_TOTAL_TIME_T is per transfer.  Tiles are fetched concurrently, so
 * summing that value does not represent the elapsed time of a segment. */
static count_time_t monotonic_time_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (count_time_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

static double segment_throughput_mbps(uint64_t bytes, count_time_t wall_us)
{
    return (wall_us > 0)
               ? ((double)bytes * 8.0 / (double)wall_us)
               : 0.0;
}

/* ── init ─────────────────────────────────────────────────────────────── */
RET request_handler_v2_init(request_handler_v2_t *self,
                            const char *ser_adrr,
                            COUNT seg_count,
                            COUNT version_count,
                            COUNT tile_count,
                            HTTP_VERSION protocol,
                            int max_parallel_downloads)
{
    if (!self || !ser_adrr)
        return RET_FAIL;

    memset(self, 0, sizeof(request_handler_v2_t));
    self->ser_addr = (char *)ser_adrr;
    self->seg_count = seg_count;
    self->tile_count = tile_count;
    self->version_count = version_count;
    self->max_parallel_downloads = max_parallel_downloads;
    self->cts_out = NULL;
    self->early_term_count = 0;

    self->pool = (http_pool_t *)malloc(sizeof(http_pool_t));
    if (!self->pool)
        return RET_FAIL;

    if (http_pool_init(self->pool, protocol, max_parallel_downloads) != RET_SUCCESS)
    {
        free(self->pool);
        return RET_FAIL;
    }

#define ALLOC(field, type)                                  \
    self->field = (type *)calloc(tile_count, sizeof(type)); \
    if (!self->field)                                       \
        goto cleanup;

    self->data = (buffer_t *)calloc(tile_count, sizeof(buffer_t));
    if (!self->data)
        goto cleanup;
    for (COUNT i = 0; i < tile_count; i++)
    {
        if (buffer_init(&self->data[i]) != RET_SUCCESS)
        {
            for (COUNT j = 0; j < i; j++)
                buffer_destroy(&self->data[j]);
            goto cleanup;
        }
    }

    ALLOC(dls, bw_t)
    ALLOC(cnnt, count_time_t)
    ALLOC(pre_trans_time, count_time_t)
    ALLOC(start_trans_time, count_time_t)
    ALLOC(total_time, count_time_t)
    ALLOC(size_dl, count_time_t)
    ALLOC(namelookup_time, count_time_t)
    ALLOC(appconnect_time, count_time_t)
    ALLOC(redirect_time, count_time_t)
    ALLOC(header_size, size_t)
    ALLOC(redirect_count, count_t)
    ALLOC(connection_reused, int)

    self->p_map = (float *)calloc(tile_count, sizeof(float));
    if (!self->p_map)
        goto cleanup;
#undef ALLOC

    return RET_SUCCESS;

cleanup:
    request_handler_v2_destroy(self);
    return RET_FAIL;
}

/* ── update_pmap ─────────────────────────────────────────────────────── */
RET request_handler_v2_update_pmap(request_handler_v2_t *self,
                                   const float *p_map_src)
{
    if (!self || !self->p_map || !p_map_src)
        return RET_FAIL;
    memcpy(self->p_map, p_map_src, self->tile_count * sizeof(float));
    return RET_SUCCESS;
}

/* ── group statistics ────────────────────────────────────────────────── */
static void calc_group_stats(request_handler_v2_t *self,
                             int *tile_ids, int num,
                             tile_group_stats_t *s)
{
    memset(s, 0, sizeof(*s));
    s->min_download_speed = UINT64_MAX;

    count_time_t *ta = (num > 1)
                           ? (count_time_t *)malloc(num * sizeof(count_time_t))
                           : NULL;

    for (int i = 0; i < num; i++)
    {
        int tid = tile_ids[i];
        if (self->size_dl[tid] == 0)
            continue;

        s->total_namelookup_time += self->namelookup_time[tid];
        s->total_connect_time += self->cnnt[tid];
        s->total_appconnect_time += self->appconnect_time[tid];
        s->total_pretransfer_time += self->pre_trans_time[tid];
        s->total_starttransfer_time += self->start_trans_time[tid];
        s->total_time += self->total_time[tid];
        s->avg_download_speed += self->dls[tid];
        if (self->dls[tid] < s->min_download_speed)
            s->min_download_speed = self->dls[tid];
        if (self->dls[tid] > s->max_download_speed)
            s->max_download_speed = self->dls[tid];
        s->total_size += self->size_dl[tid];
        s->avg_header_size += self->header_size[tid];
        s->num_connections_reused += self->connection_reused[tid];
        s->total_redirects += self->redirect_count[tid];
        s->avg_redirect_time_ms += self->redirect_time[tid] / 1000.0;
        if (ta)
            ta[s->tile_count] = self->total_time[tid];
        s->tile_count++;
    }

    if (s->tile_count > 0)
    {
        s->avg_namelookup_time = s->total_namelookup_time / s->tile_count;
        s->avg_connect_time = s->total_connect_time / s->tile_count;
        s->avg_appconnect_time = s->total_appconnect_time / s->tile_count;
        s->avg_pretransfer_time = s->total_pretransfer_time / s->tile_count;
        s->avg_starttransfer_time = s->total_starttransfer_time / s->tile_count;
        s->avg_total_time = s->total_time / s->tile_count;
        s->avg_download_speed = s->avg_download_speed / s->tile_count;
        s->avg_size = s->total_size / s->tile_count;
        s->avg_header_size = s->avg_header_size / s->tile_count;
        s->avg_redirect_time_ms = s->avg_redirect_time_ms / s->tile_count;

        if (s->tile_count > 1 && ta)
        {
            double var = 0.0, mean = (double)s->avg_total_time;
            for (int i = 0; i < s->tile_count; i++)
            {
                double d = (double)ta[i] - mean;
                var += d * d;
            }
            s->jitter_ms = sqrt(var / s->tile_count) / 1000.0;
        }
    }
    if (ta)
        free(ta);
}

/* ── [FIX-A/B/C]  is_tile_in_actual_viewport ────────────────────────── *
 *
 * Canonical ground-truth viewport test.
 *
 * Coordinate system: yaw ∈ [0, 360), pitch ∈ [-90, 90].
 * Tile centre:
 *   ty = (col + 0.5) * TILE_WIDTH
 *   tp = -90 + (row + 0.5) * TILE_HEIGHT
 *
 * A tile is "in viewport" when the shortest angular distance from the
 * tile centre to the actual gaze direction is within the half-FOV.
 *
 * Shortest circular yaw distance (handles the 0°/360° seam):
 *   dy_raw = |norm_yaw − ty|
 *   dy     = (dy_raw > 180) ? 360 − dy_raw : dy_raw
 *
 * The comparison uses strict half-extents (45°×45°) so tiles that are
 * partly inside the FOV but whose CENTRE is outside are counted as
 * non-viewport — this matches the original intent of the code while
 * being geometrically correct.
 */
static bool is_tile_in_actual_viewport(int tile_id,
                                       float actual_yaw,
                                       float actual_pitch)
{
    int col = tile_id % NO_OF_COLS;
    int row = tile_id / NO_OF_COLS;

    /* [FIX-B] Always normalise the incoming gaze direction */
    float ny = wrap_angle_360(actual_yaw);
    float np = clamp_pitch(actual_pitch);

    /* [FIX-A] Tile centre in the unified [0,360) × [-90,90] system */
    float ty = (col + 0.5f) * TILE_WIDTH;
    float tp = -90.0f + (row + 0.5f) * TILE_HEIGHT;

    /* [FIX-A] Shortest circular yaw distance */
    float dy_raw = fabsf(ny - ty);
    float dy = (dy_raw > 180.0f) ? (360.0f - dy_raw) : dy_raw;

    float dp = fabsf(np - tp);

    /* [FIX-C] 90×90° viewport → half-extents of 45° each */
    return (dy <= VP_HALF_YAW && dp <= VP_HALF_PITCH);
}

/* ── post_get_info ───────────────────────────────────────────────────── */
RET request_handler_v2_post_get_info(request_handler_v2_t *self,
                                     COUNT chunk_id,
                                     int *vp_tiles,
                                     int num_vp_tiles,
                                     int *chosen_versions,
                                     float actual_yaw,
                                     float actual_pitch,
                                     HTTP_VERSION protocol,
                                     resource_monitor_t *rm)
{
    if (!self || !self->pool)
        return RET_FAIL;

    const char *pname = (protocol == STREAM_HTTP_2_0) ? "HTTP/2" : (protocol == STREAM_HTTP_3_0) ? "HTTP/3"
                                                                                                 : "HTTP/1.1";

    printf("\n========== SEGMENT %llu (%s) ==========\n",
           chunk_id + 1, pname);

    /* build non-VP list */
    int non_vp[NO_OF_ROWS * NO_OF_COLS], nvc = 0;
    for (COUNT tid = 0; tid < self->tile_count; tid++)
    {
        bool isvp = false;
        for (int i = 0; i < num_vp_tiles; i++)
            if (vp_tiles[i] == (int)tid)
            {
                isvp = true;
                break;
            }
        if (!isvp)
            non_vp[nvc++] = (int)tid;
    }

    int total_jobs = num_vp_tiles + nvc;
    download_task_t *jobs = (download_task_t *)calloc(total_jobs, sizeof(*jobs));
    char **urls = (char **)calloc(total_jobs, sizeof(char *));
    if (!jobs || !urls)
    {
        free(jobs);
        free(urls);
        return RET_FAIL;
    }

    char ubuf[512];

    /* viewport tiles */
    for (int i = 0; i < num_vp_tiles; i++)
    {
        int tid = vp_tiles[i];
        int ver = (self->cts_out)
                      ? self->cts_out->tiles[tid].chosen_version
                      : (chosen_versions ? chosen_versions[i] : 0);
        int qp = tile_version_to_num(ver);

        snprintf(ubuf, sizeof(ubuf),
                 "%s/" VIDEO_PATH_PREFIX "_%llu"
                 "/erp_8x6/tile_yuv/tile_%d_%d_" VIDEO_TILE_RESOLUTION "_QP%d.bin",
                 self->ser_addr, chunk_id + 1,
                 tid / NO_OF_COLS, tid % NO_OF_COLS, qp);

        urls[i] = strdup(ubuf);
        jobs[i].url = urls[i];
        jobs[i].tile_id = tid;
        jobs[i].status = RET_FAIL;
        jobs[i].prob_ptr = (self->p_map) ? &self->p_map[tid] : NULL;

        jobs[i].data = &self->data[tid];
        jobs[i].dls = &self->dls[tid];
        jobs[i].cnnt = &self->cnnt[tid];
        jobs[i].pre_trans_time = &self->pre_trans_time[tid];
        jobs[i].start_trans_time = &self->start_trans_time[tid];
        jobs[i].total_time = &self->total_time[tid];
        jobs[i].size_dl = &self->size_dl[tid];
        jobs[i].namelookup_time = &self->namelookup_time[tid];
        jobs[i].appconnect_time = &self->appconnect_time[tid];
        jobs[i].redirect_time = &self->redirect_time[tid];
        jobs[i].header_size = &self->header_size[tid];
        jobs[i].redirect_count = &self->redirect_count[tid];
        jobs[i].connection_reused = &self->connection_reused[tid];
    }

    /* non-viewport tiles */
    for (int i = 0; i < nvc; i++)
    {
        int tid = non_vp[i];
        int ji = num_vp_tiles + i;
        int ver = (self->cts_out)
                      ? self->cts_out->tiles[tid].chosen_version
                      : 0;
        int qp = tile_version_to_num(ver);

        snprintf(ubuf, sizeof(ubuf),
                 "%s/" VIDEO_PATH_PREFIX "_%llu"
                 "/erp_8x6/tile_yuv/tile_%d_%d_" VIDEO_TILE_RESOLUTION "_QP%d.bin",
                 self->ser_addr, chunk_id + 1,
                 tid / NO_OF_COLS, tid % NO_OF_COLS, qp);

        urls[ji] = strdup(ubuf);
        jobs[ji].url = urls[ji];
        jobs[ji].tile_id = tid;
        jobs[ji].status = RET_FAIL;
        jobs[ji].prob_ptr = (self->p_map) ? &self->p_map[tid] : NULL;

        jobs[ji].data = &self->data[tid];
        jobs[ji].dls = &self->dls[tid];
        jobs[ji].cnnt = &self->cnnt[tid];
        jobs[ji].pre_trans_time = &self->pre_trans_time[tid];
        jobs[ji].start_trans_time = &self->start_trans_time[tid];
        jobs[ji].total_time = &self->total_time[tid];
        jobs[ji].size_dl = &self->size_dl[tid];
        jobs[ji].namelookup_time = &self->namelookup_time[tid];
        jobs[ji].appconnect_time = &self->appconnect_time[tid];
        jobs[ji].redirect_time = &self->redirect_time[tid];
        jobs[ji].header_size = &self->header_size[tid];
        jobs[ji].redirect_count = &self->redirect_count[tid];
        jobs[ji].connection_reused = &self->connection_reused[tid];
    }

    printf("[POOL] %d VP + %d non-VP tiles (tau=%.3f)\n",
           num_vp_tiles, nvc, self->pool->early_term_tau);

    count_time_t segment_start_us = monotonic_time_us();

    // if (http_pool_get_parallel(self->pool, jobs, total_jobs,
    //                            self->max_parallel_downloads) != RET_SUCCESS) {
    //     fprintf(stderr, "[rh_v2] download failed\n");
    //     for (int i = 0; i < total_jobs; i++) free(urls[i]);
    //     free(urls); free(jobs);
    //     return RET_FAIL;
    // }

    if (http_pool_get_parallel_dynamic(self->pool, jobs, total_jobs,
                                       self->max_parallel_downloads, rm) != RET_SUCCESS)
    {
        // if (http_pool_get_parallel(self->pool, jobs, total_jobs,
        //                            self->max_parallel_downloads) != RET_SUCCESS) {
        fprintf(stderr, "[rh_v2] download failed\n");
        for (int i = 0; i < total_jobs; i++)
            free(urls[i]);
        free(urls);
        free(jobs);
        return RET_FAIL;
    }

    count_time_t segment_end_us = monotonic_time_us();
    count_time_t segment_wall_us =
        (segment_end_us > segment_start_us)
            ? (segment_end_us - segment_start_us)
            : 0;

    /* Count transfers cancelled by ET separately from requests that the
     * adaptive resource gate never admitted. */
    int et = 0;
    int gated = 0;
    for (int i = 0; i < total_jobs; i++)
    {
        if (jobs[i].status == RET_EARLY_TERMINATED)
        {
            et++;
            if (self->cts_out)
                ((cts_tile_t *)&self->cts_out->tiles[jobs[i].tile_id])
                    ->early_terminated = 1;
        }
        else if (jobs[i].status == RET_RESOURCE_GATED)
        {
            gated++;
        }
    }
    self->early_term_count += et;
    if (et)
        printf("[ET] %d tile(s) early-terminated this segment "
               "(total=%d)\n",
               et, self->early_term_count);
    if (gated)
        printf("[STAGE-2] %d tile(s) left deferred by the PSI gate\n", gated);

    /* statistics */
    tile_group_stats_t vp_s, nv_s;
    calc_group_stats(self, vp_tiles, num_vp_tiles, &vp_s);
    calc_group_stats(self, non_vp, nvc, &nv_s);

    int64_t total_bytes = (int64_t)vp_s.total_size + (int64_t)nv_s.total_size;
    int all_tiles = vp_s.tile_count + nv_s.tile_count;
    int all_reused = vp_s.num_connections_reused + nv_s.num_connections_reused;

    printf("[SEG %llu] Proto=%s Tiles=%d(VP:%d NV:%d ET:%d Gated:%d) "
           "Size=%.2fMB Time=%.2fms Tput=%.2fMbps Reuse=%.1f%%\n",
           chunk_id + 1, pname,
           all_tiles, vp_s.tile_count, nv_s.tile_count, et, gated,
           total_bytes / 1048576.0,
           segment_wall_us / 1000.0,
           segment_throughput_mbps((uint64_t)total_bytes, segment_wall_us),
           (all_tiles > 0) ? (all_reused * 100.0 / all_tiles) : 0.0);
    printf("====================================\n\n");

    /* ── Wasted Ratio and QoE Drop Rate ─────────────────────────────── *
     *
     * Wasted bytes: downloaded but centre NOT in actual 90×90° viewport.
     * QoE drop: tile centre IS in viewport AND was early-terminated.
     *
     * Both use the fixed is_tile_in_actual_viewport() with normalised
     * coords and correct circular-distance yaw check.
     */
    long long total_dl = 0, wasted = 0;
    int actual_vp_count = 0, qoe_dropped = 0;

    for (int i = 0; i < (int)self->tile_count; i++)
    {
        bool in_vp = is_tile_in_actual_viewport(i, actual_yaw, actual_pitch);
        if (in_vp)
            actual_vp_count++;

        if (self->size_dl[i] > 0)
        {
            total_dl += self->size_dl[i];
            if (!in_vp)
                wasted += self->size_dl[i];
        }

        if (in_vp && self->cts_out && self->cts_out->tiles[i].early_terminated)
            qoe_dropped++;
    }

    float wasted_ratio = (total_dl > 0) ? ((float)wasted / total_dl) : 0.0f;
    float qoe_drop_rate = (actual_vp_count > 0) ? ((float)qoe_dropped / actual_vp_count) : 0.0f;

    printf("[METRICS] Wasted Ratio: %.2f%% | QoE Drop Rate: %.2f%%\n",
           wasted_ratio * 100.0f, qoe_drop_rate * 100.0f);

    /* CSV */
    FILE *csv = fopen("http_metrics_v2.csv", "a");
    if (csv)
    {
        if (ftell(csv) == 0)
            fprintf(csv, "seg,proto,type,tiles,size_mb,speed_mbps,"
                         "total_ms,jitter_ms,reuse_pct,et\n");
        fprintf(csv, "%llu,%s,viewport,%d,%.4f,%.4f,%.4f,%.2f,%.1f,%d\n",
                chunk_id + 1, pname, vp_s.tile_count,
                vp_s.total_size / 1048576.0,
                segment_throughput_mbps(vp_s.total_size, segment_wall_us),
                segment_wall_us / 1000.0, vp_s.jitter_ms,
                (vp_s.tile_count > 0) ? (vp_s.num_connections_reused * 100.0 / vp_s.tile_count) : 0.0,
                et);
        fprintf(csv, "%llu,%s,non-vp,%d,%.4f,%.4f,%.4f,%.2f,%.1f,0\n",
                chunk_id + 1, pname, nv_s.tile_count,
                nv_s.total_size / 1048576.0,
                segment_throughput_mbps(nv_s.total_size, segment_wall_us),
                segment_wall_us / 1000.0, nv_s.jitter_ms,
                (nv_s.tile_count > 0) ? (nv_s.num_connections_reused * 100.0 / nv_s.tile_count) : 0.0);
        fprintf(csv, "%llu,%s,summary,%.4f,%.4f\n",
                chunk_id + 1, pname, wasted_ratio, qoe_drop_rate);
        fclose(csv);
    }

    for (int i = 0; i < total_jobs; i++)
        free(urls[i]);
    free(urls);
    free(jobs);
    return RET_SUCCESS;
}

/* ── Two-Stage Dispatch ─────────────────────────────────────────────────
 *
 * Stage 1 — HIGH PRIORITY (core_tiles[0..n_core-1]):
 *   Các tile thuộc vùng 90×90° dự đoán được tải NGAY LẬP TỨC, không bị
 *   Early Terminate dù p_i rớt xuống.  early_term_tau được đặt = 0 khi
 *   poll Stage 1 để đảm bảo điều này.
 *
 * Stage 2 — RESOURCE PROBING + ET (các tile còn lại trong vp_tiles,
 *   không thuộc core):
 *   Được dispatch SAU KHI Stage 1 hoàn thành.  early_term_tau của pool
 *   được khôi phục; bất kỳ tile Stage 2 nào có *prob_ptr < tau_final sẽ
 *   bị RESET_STREAM.
 *
 * Tham số:
 *   core_tiles / n_core  — mảng tile ID lõi 90×90° (Stage 1)
 *   vp_tiles   / n_vp    — toàn bộ tile có p_i >= 0.15 từ build_vp_list
 *                           (bao gồm cả core; hàm tự lọc phần giao)
 * ----------------------------------------------------------------------- */
static RET request_handler_v2_post_get_info_two_stage_legacy(
        request_handler_v2_t *self,
        COUNT chunk_id,
        int  *core_tiles,   int n_core,
        int  *vp_tiles,     int num_vp_tiles,
        int  *chosen_versions,
        float actual_yaw,   float actual_pitch,
        HTTP_VERSION protocol)
{
    if (!self || !self->pool)
        return RET_FAIL;

    const char *pname = (protocol == STREAM_HTTP_2_0) ? "HTTP/2"
                      : (protocol == STREAM_HTTP_3_0) ? "HTTP/3"
                      : "HTTP/1.1";

    printf("\n========== SEGMENT %llu (%s) [Two-Stage] ==========\n",
           chunk_id + 1, pname);

    /* ── Xây dựng lookup để kiểm tra tile có thuộc core không ── */
    bool is_core[NO_OF_ROWS * NO_OF_COLS];
    memset(is_core, 0, sizeof(is_core));
    for (int i = 0; i < n_core; i++)
        if (core_tiles[i] >= 0 && core_tiles[i] < (int)(NO_OF_ROWS * NO_OF_COLS))
            is_core[core_tiles[i]] = true;

    /* ── Xây dựng Stage 2: vp_tiles \ core_tiles ── */
    int stage2_tiles[NO_OF_ROWS * NO_OF_COLS];
    int n_stage2 = 0;
    for (int i = 0; i < num_vp_tiles; i++) {
        if (!is_core[vp_tiles[i]])
            stage2_tiles[n_stage2++] = vp_tiles[i];
    }

    /* ── Xây dựng danh sách non-VP (tiles không trong vp_tiles nào) ── */
    bool in_vp_list[NO_OF_ROWS * NO_OF_COLS];
    memset(in_vp_list, 0, sizeof(in_vp_list));
    for (int i = 0; i < num_vp_tiles; i++)
        if (vp_tiles[i] >= 0)
            in_vp_list[vp_tiles[i]] = true;

    int non_vp[NO_OF_ROWS * NO_OF_COLS], nvc = 0;
    for (COUNT tid = 0; tid < self->tile_count; tid++)
        if (!in_vp_list[tid])
            non_vp[nvc++] = (int)tid;

    count_time_t segment_start_us = monotonic_time_us();

    /* ─────────────────────────────────────────────────────────────────
     * STAGE 1: Tải core tiles NGAY LẬP TỨC, không Early Terminate
     * ───────────────────────────────────────────────────────────────── */
    printf("[STAGE-1] Dispatching %d core tiles (90x90, no ET)...\n", n_core);

    if (n_core > 0) {
        download_task_t *s1_jobs = (download_task_t *)calloc(n_core, sizeof(*s1_jobs));
        char           **s1_urls = (char **)calloc(n_core, sizeof(char *));
        if (!s1_jobs || !s1_urls) {
            free(s1_jobs); free(s1_urls);
            return RET_FAIL;
        }

        char ubuf[512];
        for (int i = 0; i < n_core; i++) {
            int tid = core_tiles[i];
            int ver = (self->cts_out)
                      ? self->cts_out->tiles[tid].chosen_version
                      : (chosen_versions ? chosen_versions[i] : 0);
            int qp  = tile_version_to_num(ver);

            snprintf(ubuf, sizeof(ubuf),
                     "%s/" VIDEO_PATH_PREFIX "_%llu"
                     "/erp_8x6/tile_yuv/tile_%d_%d_" VIDEO_TILE_RESOLUTION "_QP%d.bin",
                     self->ser_addr, chunk_id + 1,
                     tid / NO_OF_COLS, tid % NO_OF_COLS, qp);


            s1_urls[i]             = strdup(ubuf);
            s1_jobs[i].url         = s1_urls[i];
            s1_jobs[i].tile_id     = tid;
            s1_jobs[i].status      = RET_FAIL;
            /* Stage 1 KHÔNG dùng ET: prob_ptr = NULL */
            s1_jobs[i].prob_ptr    = NULL;

            s1_jobs[i].data              = &self->data[tid];
            s1_jobs[i].dls               = &self->dls[tid];
            s1_jobs[i].cnnt              = &self->cnnt[tid];
            s1_jobs[i].pre_trans_time    = &self->pre_trans_time[tid];
            s1_jobs[i].start_trans_time  = &self->start_trans_time[tid];
            s1_jobs[i].total_time        = &self->total_time[tid];
            s1_jobs[i].size_dl           = &self->size_dl[tid];
            s1_jobs[i].namelookup_time   = &self->namelookup_time[tid];
            s1_jobs[i].appconnect_time   = &self->appconnect_time[tid];
            s1_jobs[i].redirect_time     = &self->redirect_time[tid];
            s1_jobs[i].header_size       = &self->header_size[tid];
            s1_jobs[i].redirect_count    = &self->redirect_count[tid];
            s1_jobs[i].connection_reused = &self->connection_reused[tid];
        }

        /* Tạm thời vô hiệu hoá ET cho Stage 1 */
        float saved_tau = self->pool->early_term_tau;
        self->pool->early_term_tau = 0.0f;

        RET s1_ret = http_pool_get_parallel(self->pool, s1_jobs, n_core,
                                            self->max_parallel_downloads);

        /* Khôi phục tau cho Stage 2 */
        self->pool->early_term_tau = saved_tau;

        for (int i = 0; i < n_core; i++) free(s1_urls[i]);
        free(s1_urls);
        free(s1_jobs);

        if (s1_ret != RET_SUCCESS) {
            fprintf(stderr, "[rh_v2] Stage 1 download failed\n");
            return RET_FAIL;
        }
        printf("[STAGE-1] Done.\n");
    }

    /* ─────────────────────────────────────────────────────────────────
     * STAGE 2: Tải các tile còn lại (Stage 2 + non-VP), áp dụng ET
     * ───────────────────────────────────────────────────────────────── */
    int n_stage2_total = n_stage2 + nvc;
    printf("[STAGE-2] Dispatching %d tiles (stage2=%d + non-vp=%d, ET=%.3f)...\n",
           n_stage2_total, n_stage2, nvc, self->pool->early_term_tau);

    int et = 0;   /* early-terminated counter (chỉ tính Stage 2) */

    if (n_stage2_total > 0) {
        download_task_t *s2_jobs = (download_task_t *)calloc(n_stage2_total, sizeof(*s2_jobs));
        char           **s2_urls = (char **)calloc(n_stage2_total, sizeof(char *));
        if (!s2_jobs || !s2_urls) {
            free(s2_jobs); free(s2_urls);
            return RET_FAIL;
        }

        char ubuf[512];

        /* Stage 2A: các vp_tiles ngoài core */
        for (int i = 0; i < n_stage2; i++) {
            int tid = stage2_tiles[i];
            int ver = (self->cts_out)
                      ? self->cts_out->tiles[tid].chosen_version
                      : 0;
            int qp  = tile_version_to_num(ver);

            snprintf(ubuf, sizeof(ubuf),
                     "%s/" VIDEO_PATH_PREFIX "_%llu"
                     "/erp_8x6/tile_yuv/tile_%d_%d_" VIDEO_TILE_RESOLUTION "_QP%d.bin",
                     self->ser_addr, chunk_id + 1,
                     tid / NO_OF_COLS, tid % NO_OF_COLS, qp);

            s2_urls[i]             = strdup(ubuf);
            s2_jobs[i].url         = s2_urls[i];
            s2_jobs[i].tile_id     = tid;
            s2_jobs[i].status      = RET_FAIL;
            /* Stage 2 DÙNG ET: gắn prob_ptr để polling vòng lặp có thể cắt */
            s2_jobs[i].prob_ptr    = (self->p_map) ? &self->p_map[tid] : NULL;

            s2_jobs[i].data              = &self->data[tid];
            s2_jobs[i].dls               = &self->dls[tid];
            s2_jobs[i].cnnt              = &self->cnnt[tid];
            s2_jobs[i].pre_trans_time    = &self->pre_trans_time[tid];
            s2_jobs[i].start_trans_time  = &self->start_trans_time[tid];
            s2_jobs[i].total_time        = &self->total_time[tid];
            s2_jobs[i].size_dl           = &self->size_dl[tid];
            s2_jobs[i].namelookup_time   = &self->namelookup_time[tid];
            s2_jobs[i].appconnect_time   = &self->appconnect_time[tid];
            s2_jobs[i].redirect_time     = &self->redirect_time[tid];
            s2_jobs[i].header_size       = &self->header_size[tid];
            s2_jobs[i].redirect_count    = &self->redirect_count[tid];
            s2_jobs[i].connection_reused = &self->connection_reused[tid];
        }

        /* Stage 2B: non-VP tiles */
        for (int i = 0; i < nvc; i++) {
            int tid = non_vp[i];
            int ji  = n_stage2 + i;
            int ver = (self->cts_out)
                      ? self->cts_out->tiles[tid].chosen_version
                      : 0;
            int qp  = tile_version_to_num(ver);

            snprintf(ubuf, sizeof(ubuf),
                     "%s/" VIDEO_PATH_PREFIX "_%llu"
                     "/erp_8x6/tile_yuv/tile_%d_%d_" VIDEO_TILE_RESOLUTION "_QP%d.bin",
                     self->ser_addr, chunk_id + 1,
                     tid / NO_OF_COLS, tid % NO_OF_COLS, qp);

            s2_urls[ji]             = strdup(ubuf);
            s2_jobs[ji].url         = s2_urls[ji];
            s2_jobs[ji].tile_id     = tid;
            s2_jobs[ji].status      = RET_FAIL;
            s2_jobs[ji].prob_ptr    = (self->p_map) ? &self->p_map[tid] : NULL;

            s2_jobs[ji].data              = &self->data[tid];
            s2_jobs[ji].dls               = &self->dls[tid];
            s2_jobs[ji].cnnt              = &self->cnnt[tid];
            s2_jobs[ji].pre_trans_time    = &self->pre_trans_time[tid];
            s2_jobs[ji].start_trans_time  = &self->start_trans_time[tid];
            s2_jobs[ji].total_time        = &self->total_time[tid];
            s2_jobs[ji].size_dl           = &self->size_dl[tid];
            s2_jobs[ji].namelookup_time   = &self->namelookup_time[tid];
            s2_jobs[ji].appconnect_time   = &self->appconnect_time[tid];
            s2_jobs[ji].redirect_time     = &self->redirect_time[tid];
            s2_jobs[ji].header_size       = &self->header_size[tid];
            s2_jobs[ji].redirect_count    = &self->redirect_count[tid];
            s2_jobs[ji].connection_reused = &self->connection_reused[tid];
        }

        /* Dùng http_pool_get_parallel (ET được nhúng vào polling loop trong
         * http_pool_get_parallel_et_only — xem bên dưới).
         * Nếu muốn dùng hàm có sẵn, dùng http_pool_get_parallel và kiểm tra
         * prob_ptr TRƯỚC khi dispatch (pre-filter ET). */
        RET s2_ret = http_pool_get_parallel_et(self->pool, s2_jobs, n_stage2_total,
                                               self->max_parallel_downloads);

        /* Đếm ET từ Stage 2 */
        for (int i = 0; i < n_stage2_total; i++) {
            if (s2_jobs[i].status == RET_EARLY_TERMINATED) {
                et++;
                if (self->cts_out)
                    ((cts_tile_t *)&self->cts_out->tiles[s2_jobs[i].tile_id])
                        ->early_terminated = 1;
            }
        }

        for (int i = 0; i < n_stage2_total; i++) free(s2_urls[i]);
        free(s2_urls);
        free(s2_jobs);

        if (s2_ret != RET_SUCCESS)
            fprintf(stderr, "[rh_v2] Stage 2 download failed (some ET may apply)\n");
    }

    count_time_t segment_end_us = monotonic_time_us();
    count_time_t segment_wall_us =
        (segment_end_us > segment_start_us)
            ? (segment_end_us - segment_start_us)
            : 0;

    self->early_term_count += et;
    if (et)
        printf("[ET] %d Stage-2 tile(s) early-terminated (total=%d)\n",
               et, self->early_term_count);

    /* ── Statistics (dùng lại core_tiles như "vp_tiles" cho nhóm VP) ── */
    tile_group_stats_t vp_s, nv_s;
    calc_group_stats(self, core_tiles, n_core, &vp_s);
    calc_group_stats(self, non_vp, nvc, &nv_s);

    int64_t      total_bytes = (int64_t)vp_s.total_size + (int64_t)nv_s.total_size;
    int          all_tiles   = vp_s.tile_count  + nv_s.tile_count;
    int          all_reused  = vp_s.num_connections_reused + nv_s.num_connections_reused;

    printf("[SEG %llu] Proto=%s Tiles=%d(Core:%d S2:%d NV:%d ET:%d) "
           "Size=%.2fMB Time=%.2fms Tput=%.2fMbps Reuse=%.1f%%\n",
           chunk_id + 1, pname,
           all_tiles, vp_s.tile_count, n_stage2, nv_s.tile_count, et,
           total_bytes / 1048576.0,
           segment_wall_us / 1000.0,
           segment_throughput_mbps((uint64_t)total_bytes, segment_wall_us),
           (all_tiles > 0) ? (all_reused * 100.0 / all_tiles) : 0.0);
    printf("====================================\n\n");

    /* Wasted ratio & QoE drop (giữ nguyên logic cũ) */
    long long total_dl = 0, wasted = 0;
    int actual_vp_count = 0, qoe_dropped = 0;

    for (int i = 0; i < (int)self->tile_count; i++) {
        bool in_vp = is_tile_in_actual_viewport(i, actual_yaw, actual_pitch);
        if (in_vp) actual_vp_count++;

        if (self->size_dl[i] > 0) {
            total_dl += self->size_dl[i];
            if (!in_vp) wasted += self->size_dl[i];
        }
        if (in_vp && self->cts_out && self->cts_out->tiles[i].early_terminated)
            qoe_dropped++;
    }

    float wasted_ratio  = (total_dl > 0)         ? ((float)wasted     / total_dl)         : 0.0f;
    float qoe_drop_rate = (actual_vp_count > 0)   ? ((float)qoe_dropped / actual_vp_count) : 0.0f;

    printf("[METRICS] Wasted Ratio: %.2f%% | QoE Drop Rate: %.2f%%\n",
           wasted_ratio * 100.0f, qoe_drop_rate * 100.0f);

    /* CSV */
    FILE *csv = fopen("http_metrics_v2.csv", "a");
    if (csv) {
        if (ftell(csv) == 0)
            fprintf(csv, "seg,proto,type,tiles,size_mb,speed_mbps,"
                         "total_ms,jitter_ms,reuse_pct,et\n");
        fprintf(csv, "%llu,%s,core-vp,%d,%.4f,%.4f,%.4f,%.2f,%.1f,0\n",
                chunk_id + 1, pname, vp_s.tile_count,
                vp_s.total_size / 1048576.0,
                segment_throughput_mbps(vp_s.total_size, segment_wall_us),
                segment_wall_us / 1000.0, vp_s.jitter_ms,
                (vp_s.tile_count > 0)
                    ? (vp_s.num_connections_reused * 100.0 / vp_s.tile_count)
                    : 0.0);
        fprintf(csv, "%llu,%s,non-vp,%d,%.4f,%.4f,%.4f,%.2f,%.1f,0\n",
                chunk_id + 1, pname, nv_s.tile_count,
                nv_s.total_size / 1048576.0,
                segment_throughput_mbps(nv_s.total_size, segment_wall_us),
                segment_wall_us / 1000.0, nv_s.jitter_ms,
                (nv_s.tile_count > 0)
                    ? (nv_s.num_connections_reused * 100.0 / nv_s.tile_count)
                    : 0.0);
        fprintf(csv, "%llu,%s,summary,%.4f,%.4f\n",
                chunk_id + 1, pname, wasted_ratio, qoe_drop_rate);
        fclose(csv);
    }

    return RET_SUCCESS;
}

/* Adaptive PSI-gated two-stage entry point. */
RET request_handler_v2_post_get_info_two_stage(
        request_handler_v2_t *self,
        COUNT chunk_id,
        int  *core_tiles,   int n_core,
        int  *vp_tiles,     int num_vp_tiles,
        int  *chosen_versions,
        float actual_yaw,   float actual_pitch,
        HTTP_VERSION protocol,
        resource_monitor_t *rm)
{
    if (rm) {
        /* The dynamic pool owns the stage boundary: p_i >= 0.50 is sent
         * immediately and p_i < 0.50 is PSI-gated while Stage 1 is active. */
        return request_handler_v2_post_get_info(self, chunk_id,
                                                vp_tiles, num_vp_tiles,
                                                chosen_versions,
                                                actual_yaw, actual_pitch,
                                                protocol, rm);
    }

    /* Compatibility fallback for callers that do not provide PSI. */
    return request_handler_v2_post_get_info_two_stage_legacy(
            self, chunk_id, core_tiles, n_core,
            vp_tiles, num_vp_tiles, chosen_versions,
            actual_yaw, actual_pitch, protocol);
}

/* Reset per-segment request state. */
RET request_handler_v2_reset(request_handler_v2_t *self)
{
    if (!self)
        return RET_FAIL;
    for (COUNT i = 0; i < self->tile_count; i++)
    {
        buffer_destroy(&self->data[i]);
        buffer_init(&self->data[i]);
        self->dls[i] = self->cnnt[i] = self->pre_trans_time[i] = 0;
        self->start_trans_time[i] = self->total_time[i] = self->size_dl[i] = 0;
        self->namelookup_time[i] = self->appconnect_time[i] = 0;
        self->redirect_time[i] = 0;
        self->header_size[i] = 0;
        self->redirect_count[i] = 0;
        self->connection_reused[i] = 0;
        /* p_map is preserved — holds latest estimate */
    }
    self->cts_out = NULL;
    return RET_SUCCESS;
}

RET request_handler_v2_post_get_info_no_rm(request_handler_v2_t *self,
                                           COUNT chunk_id,
                                           int *vp_tiles,
                                           int num_vp_tiles,
                                           int *chosen_versions,
                                           float actual_yaw,
                                           float actual_pitch,
                                           HTTP_VERSION protocol)
{
    if (!self || !self->pool)
        return RET_FAIL;

    const char *pname = (protocol == STREAM_HTTP_2_0) ? "HTTP/2" : (protocol == STREAM_HTTP_3_0) ? "HTTP/3"
                                                                                                 : "HTTP/1.1";

    printf("\n========== SEGMENT %llu (%s) ==========\n",
           chunk_id + 1, pname);

    /* build non-VP list */
    int non_vp[NO_OF_ROWS * NO_OF_COLS], nvc = 0;
    for (COUNT tid = 0; tid < self->tile_count; tid++)
    {
        bool isvp = false;
        for (int i = 0; i < num_vp_tiles; i++)
            if (vp_tiles[i] == (int)tid)
            {
                isvp = true;
                break;
            }
        if (!isvp)
            non_vp[nvc++] = (int)tid;
    }

    int total_jobs = num_vp_tiles + nvc;
    download_task_t *jobs = (download_task_t *)calloc(total_jobs, sizeof(*jobs));
    char **urls = (char **)calloc(total_jobs, sizeof(char *));
    if (!jobs || !urls)
    {
        free(jobs);
        free(urls);
        return RET_FAIL;
    }

    char ubuf[512];

    /* viewport tiles */
    for (int i = 0; i < num_vp_tiles; i++)
    {
        int tid = vp_tiles[i];
        int ver = (self->cts_out)
                      ? self->cts_out->tiles[tid].chosen_version
                      : (chosen_versions ? chosen_versions[i] : 0);
        int qp = tile_version_to_num(ver);

        snprintf(ubuf, sizeof(ubuf),
                 "%s/" VIDEO_PATH_PREFIX "_%llu"
                 "/erp_8x6/tile_yuv/tile_%d_%d_" VIDEO_TILE_RESOLUTION "_QP%d.bin",
                 self->ser_addr, chunk_id + 1,
                 tid / NO_OF_COLS, tid % NO_OF_COLS, qp);

        urls[i] = strdup(ubuf);
        jobs[i].url = urls[i];
        jobs[i].tile_id = tid;
        jobs[i].status = RET_FAIL;
        jobs[i].prob_ptr = (self->p_map) ? &self->p_map[tid] : NULL;

        jobs[i].data = &self->data[tid];
        jobs[i].dls = &self->dls[tid];
        jobs[i].cnnt = &self->cnnt[tid];
        jobs[i].pre_trans_time = &self->pre_trans_time[tid];
        jobs[i].start_trans_time = &self->start_trans_time[tid];
        jobs[i].total_time = &self->total_time[tid];
        jobs[i].size_dl = &self->size_dl[tid];
        jobs[i].namelookup_time = &self->namelookup_time[tid];
        jobs[i].appconnect_time = &self->appconnect_time[tid];
        jobs[i].redirect_time = &self->redirect_time[tid];
        jobs[i].header_size = &self->header_size[tid];
        jobs[i].redirect_count = &self->redirect_count[tid];
        jobs[i].connection_reused = &self->connection_reused[tid];
    }

    /* non-viewport tiles */
    for (int i = 0; i < nvc; i++)
    {
        int tid = non_vp[i];
        int ji = num_vp_tiles + i;
        int ver = (self->cts_out)
                      ? self->cts_out->tiles[tid].chosen_version
                      : 0;
        int qp = tile_version_to_num(ver);

        snprintf(ubuf, sizeof(ubuf),
                 "%s/" VIDEO_PATH_PREFIX "_%llu"
                 "/erp_8x6/tile_yuv/tile_%d_%d_" VIDEO_TILE_RESOLUTION "_QP%d.bin",
                 self->ser_addr, chunk_id + 1,
                 tid / NO_OF_COLS, tid % NO_OF_COLS, qp);

        urls[ji] = strdup(ubuf);
        jobs[ji].url = urls[ji];
        jobs[ji].tile_id = tid;
        jobs[ji].status = RET_FAIL;
        jobs[ji].prob_ptr = (self->p_map) ? &self->p_map[tid] : NULL;

        jobs[ji].data = &self->data[tid];
        jobs[ji].dls = &self->dls[tid];
        jobs[ji].cnnt = &self->cnnt[tid];
        jobs[ji].pre_trans_time = &self->pre_trans_time[tid];
        jobs[ji].start_trans_time = &self->start_trans_time[tid];
        jobs[ji].total_time = &self->total_time[tid];
        jobs[ji].size_dl = &self->size_dl[tid];
        jobs[ji].namelookup_time = &self->namelookup_time[tid];
        jobs[ji].appconnect_time = &self->appconnect_time[tid];
        jobs[ji].redirect_time = &self->redirect_time[tid];
        jobs[ji].header_size = &self->header_size[tid];
        jobs[ji].redirect_count = &self->redirect_count[tid];
        jobs[ji].connection_reused = &self->connection_reused[tid];
    }

    printf("[POOL] %d VP + %d non-VP tiles (tau=%.3f)\n",
           num_vp_tiles, nvc, self->pool->early_term_tau);

    count_time_t segment_start_us = monotonic_time_us();

    // if (http_pool_get_parallel(self->pool, jobs, total_jobs,
    //                            self->max_parallel_downloads) != RET_SUCCESS) {
    //     fprintf(stderr, "[rh_v2] download failed\n");
    //     for (int i = 0; i < total_jobs; i++) free(urls[i]);
    //     free(urls); free(jobs);
    //     return RET_FAIL;
    // }

    // if (http_pool_get_parallel_dynamic(self->pool, jobs, total_jobs,
    //                            self->max_parallel_downloads, rm) != RET_SUCCESS) {
    if (http_pool_get_parallel(self->pool, jobs, total_jobs,
                               self->max_parallel_downloads) != RET_SUCCESS)
    {
        fprintf(stderr, "[rh_v2] download failed\n");
        for (int i = 0; i < total_jobs; i++)
            free(urls[i]);
        free(urls);
        free(jobs);
        return RET_FAIL;
    }

    count_time_t segment_end_us = monotonic_time_us();
    count_time_t segment_wall_us =
        (segment_end_us > segment_start_us)
            ? (segment_end_us - segment_start_us)
            : 0;

    /* count early-terminated tiles */
    int et = 0;
    for (int i = 0; i < total_jobs; i++)
    {
        if (jobs[i].status == RET_EARLY_TERMINATED)
        {
            et++;
            if (self->cts_out)
                ((cts_tile_t *)&self->cts_out->tiles[jobs[i].tile_id])
                    ->early_terminated = 1;
        }
    }
    self->early_term_count += et;
    if (et)
        printf("[ET] %d tile(s) early-terminated this segment "
               "(total=%d)\n",
               et, self->early_term_count);

    /* statistics */
    tile_group_stats_t vp_s, nv_s;
    calc_group_stats(self, vp_tiles, num_vp_tiles, &vp_s);
    calc_group_stats(self, non_vp, nvc, &nv_s);

    int64_t total_bytes = (int64_t)vp_s.total_size + (int64_t)nv_s.total_size;
    int all_tiles = vp_s.tile_count + nv_s.tile_count;
    int all_reused = vp_s.num_connections_reused + nv_s.num_connections_reused;

    printf("[SEG %llu] Proto=%s Tiles=%d(VP:%d NV:%d ET:%d) "
           "Size=%.2fMB Time=%.2fms Tput=%.2fMbps Reuse=%.1f%%\n",
           chunk_id + 1, pname,
           all_tiles, vp_s.tile_count, nv_s.tile_count, et,
           total_bytes / 1048576.0,
           segment_wall_us / 1000.0,
           segment_throughput_mbps((uint64_t)total_bytes, segment_wall_us),
           (all_tiles > 0) ? (all_reused * 100.0 / all_tiles) : 0.0);
    printf("====================================\n\n");

    /* ── Wasted Ratio and QoE Drop Rate ─────────────────────────────── *
     *
     * Wasted bytes: downloaded but centre NOT in actual 90×90° viewport.
     * QoE drop: tile centre IS in viewport AND was early-terminated.
     *
     * Both use the fixed is_tile_in_actual_viewport() with normalised
     * coords and correct circular-distance yaw check.
     */
    long long total_dl = 0, wasted = 0;
    int actual_vp_count = 0, qoe_dropped = 0;

    for (int i = 0; i < (int)self->tile_count; i++)
    {
        bool in_vp = is_tile_in_actual_viewport(i, actual_yaw, actual_pitch);
        if (in_vp)
            actual_vp_count++;

        if (self->size_dl[i] > 0)
        {
            total_dl += self->size_dl[i];
            if (!in_vp)
                wasted += self->size_dl[i];
        }

        if (in_vp && self->cts_out && self->cts_out->tiles[i].early_terminated)
            qoe_dropped++;
    }

    float wasted_ratio = (total_dl > 0) ? ((float)wasted / total_dl) : 0.0f;
    float qoe_drop_rate = (actual_vp_count > 0) ? ((float)qoe_dropped / actual_vp_count) : 0.0f;

    printf("[METRICS] Wasted Ratio: %.2f%% | QoE Drop Rate: %.2f%%\n",
           wasted_ratio * 100.0f, qoe_drop_rate * 100.0f);

    /* CSV */
    FILE *csv = fopen("http_metrics_v2.csv", "a");
    if (csv)
    {
        if (ftell(csv) == 0)
            fprintf(csv, "seg,proto,type,tiles,size_mb,speed_mbps,"
                         "total_ms,jitter_ms,reuse_pct,et\n");
        fprintf(csv, "%llu,%s,viewport,%d,%.4f,%.4f,%.4f,%.2f,%.1f,%d\n",
                chunk_id + 1, pname, vp_s.tile_count,
                vp_s.total_size / 1048576.0,
                segment_throughput_mbps(vp_s.total_size, segment_wall_us),
                segment_wall_us / 1000.0, vp_s.jitter_ms,
                (vp_s.tile_count > 0) ? (vp_s.num_connections_reused * 100.0 / vp_s.tile_count) : 0.0,
                et);
        fprintf(csv, "%llu,%s,non-vp,%d,%.4f,%.4f,%.4f,%.2f,%.1f,0\n",
                chunk_id + 1, pname, nv_s.tile_count,
                nv_s.total_size / 1048576.0,
                segment_throughput_mbps(nv_s.total_size, segment_wall_us),
                segment_wall_us / 1000.0, nv_s.jitter_ms,
                (nv_s.tile_count > 0) ? (nv_s.num_connections_reused * 100.0 / nv_s.tile_count) : 0.0);
        fprintf(csv, "%llu,%s,summary,%.4f,%.4f\n",
                chunk_id + 1, pname, wasted_ratio, qoe_drop_rate);
        fclose(csv);
    }

    for (int i = 0; i < total_jobs; i++)
        free(urls[i]);
    free(urls);
    free(jobs);
    return RET_SUCCESS;
}

/* ── destroy ─────────────────────────────────────────────────────────── */
RET request_handler_v2_destroy(request_handler_v2_t *self)
{
    if (!self)
        return RET_FAIL;
    if (self->pool)
    {
        http_pool_destroy(self->pool);
        free(self->pool);
    }
    if (self->data)
    {
        for (COUNT i = 0; i < self->tile_count; i++)
            buffer_destroy(&self->data[i]);
        free(self->data);
    }
    free(self->dls);
    free(self->cnnt);
    free(self->pre_trans_time);
    free(self->start_trans_time);
    free(self->total_time);
    free(self->size_dl);
    free(self->namelookup_time);
    free(self->appconnect_time);
    free(self->redirect_time);
    free(self->header_size);
    free(self->redirect_count);
    free(self->connection_reused);
    free(self->p_map);
    memset(self, 0, sizeof(*self));
    return RET_SUCCESS;
}

int tile_version_to_num(int version)
{
    switch (version)
    {
    case 0:
        return 38;
    case 1:
        return 32;
    case 2:
        return 28;
    case 3:
        return 24;
    case 4:
        return 20;
    default:
        return 38;
    }
}
