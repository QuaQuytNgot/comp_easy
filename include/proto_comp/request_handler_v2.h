// #ifndef REQUEST_HANDLER_V2_H
// #define REQUEST_HANDLER_V2_H

// #include "buffer.h"
// #include "define.h"
// #include "http_pool.h"
// #include <math.h>

// typedef struct
// {
//   int tile_count;

//   // Direct CURL timing metrics (microseconds) - NO CALCULATIONS
//   count_time_t avg_namelookup_time;    // DNS lookup
//   count_time_t avg_connect_time;       // TCP connect (cumulative)
//   count_time_t avg_appconnect_time;    // SSL/TLS complete (cumulative)
//   count_time_t avg_pretransfer_time;   // Ready to transfer (cumulative)
//   count_time_t avg_starttransfer_time; // TTFB (cumulative)
//   count_time_t avg_total_time;         // Total time (cumulative)

//   count_time_t total_namelookup_time;
//   count_time_t total_connect_time;
//   count_time_t total_appconnect_time;
//   count_time_t total_pretransfer_time;
//   count_time_t total_starttransfer_time;
//   count_time_t total_time;

//   // Speed metrics (direct from CURL)
//   bw_t avg_download_speed;
//   bw_t min_download_speed;
//   bw_t max_download_speed;

//   // Size metrics
//   size_t total_size;
//   size_t avg_size;
//   size_t avg_header_size;

//   // Connection metrics
//   int num_connections_reused;
//   int total_redirects;
//   double avg_redirect_time_ms;

//   // Variability
//   double jitter_ms;

// } tile_group_stats_t;

// typedef struct request_handler_v2_t
// {
//   char *ser_addr;
//   COUNT seg_count;
//   COUNT tile_count;
//   COUNT version_count;

//   http_pool_t *pool;
//   int max_parallel_downloads;

//   // V1 arrays
//   buffer_t *data;
//   bw_t *dls;
//   count_t *redirect_count;
//   size_t *header_size;
//   count_time_t *redirect_time;
//   count_time_t *cnnt;
//   count_time_t *pre_trans_time;
//   count_time_t *start_trans_time;
//   count_time_t *total_time;
//   count_time_t *size_dl;
//   count_time_t *namelookup_time;
//   count_time_t *appconnect_time;
//   int *connection_reused;

// } request_handler_v2_t;

// RET request_handler_v2_init(request_handler_v2_t *self,
//                             const char *ser_adrr,
//                             COUNT seg_count,
//                             COUNT version_count,
//                             COUNT tile_count,
//                             HTTP_VERSION protocol,
//                             int max_parallel_downloads);

// RET request_handler_v2_post_get_info(request_handler_v2_t *self,
//                                      COUNT chunk_id,
//                                      int *vp_tiles,
//                                      int num_vp_tiles,
//                                      int *chosen_versions,
//                                      HTTP_VERSION protocol);

// RET request_handler_v2_reset(request_handler_v2_t *self);
// RET request_handler_v2_destroy(request_handler_v2_t *self);
// int tile_version_to_num(int version);

// #endif

/**
 * request_handler_v2.h  (updated — Tasks 3 & 4)
 *
 * Adds:
 *   • p_map[]       — live viewport probability array that is handed to each
 *                     download_task_t so http_pool can perform early termination
 *   • sched_output  — pointer to the stochastic scheduler's output for this
 *                     segment (tile versions come from here instead of a
 *                     fixed ABR result)
 *   • early_term_count — running count of tiles cancelled mid-download
 */

#ifndef REQUEST_HANDLER_V2_H
#define REQUEST_HANDLER_V2_H

#include "buffer.h"
#include "define.h"
#include "http_pool.h"
#include "cts_scheduler.h"
#include <math.h>
#include "resource_monitor.h"

/* tile group statistics — unchanged from original */
typedef struct
{
    int tile_count;

    count_time_t avg_namelookup_time;
    count_time_t avg_connect_time;
    count_time_t avg_appconnect_time;
    count_time_t avg_pretransfer_time;
    count_time_t avg_starttransfer_time;
    count_time_t avg_total_time;

    count_time_t total_namelookup_time;
    count_time_t total_connect_time;
    count_time_t total_appconnect_time;
    count_time_t total_pretransfer_time;
    count_time_t total_starttransfer_time;
    count_time_t total_time;

    bw_t avg_download_speed;
    bw_t min_download_speed;
    bw_t max_download_speed;

    size_t total_size;
    size_t avg_size;
    size_t avg_header_size;

    int num_connections_reused;
    int total_redirects;
    double avg_redirect_time_ms;
    double jitter_ms;
} tile_group_stats_t;

typedef struct request_handler_v2_t
{
    char *ser_addr;
    COUNT seg_count;
    COUNT tile_count;
    COUNT version_count;

    http_pool_t *pool;
    int max_parallel_downloads;

    /* per-tile metric arrays (one entry per tile_id) — unchanged */
    buffer_t *data;
    bw_t *dls;
    count_t *redirect_count;
    size_t *header_size;
    count_time_t *redirect_time;
    count_time_t *cnnt;
    count_time_t *pre_trans_time;
    count_time_t *start_trans_time;
    count_time_t *total_time;
    count_time_t *size_dl;
    count_time_t *namelookup_time;
    count_time_t *appconnect_time;
    int *connection_reused;

    /* ── new fields for stochastic scheduling ──────────────────────────── */

    /* Live probability map p_i(t) — tile_count floats.
     * Updated each segment by request_handler_v2_update_pmap().
     * Each download_task_t.prob_ptr points here so http_pool can perform
     * early termination in real time during the download. */
    float *p_map;

    /* Pointer to the CTS scheduler output for the current segment.
     * When non-NULL, chosen_version comes from cts_out->tiles[tile_id]
     * instead of the chosen_versions[] parameter. */
    const cts_output_t *cts_out;

    /* Session total of tiles cancelled by early termination */
    int early_term_count;

} request_handler_v2_t;

RET request_handler_v2_init(request_handler_v2_t *self,
                            const char *ser_adrr,
                            COUNT seg_count,
                            COUNT version_count,
                            COUNT tile_count,
                            HTTP_VERSION protocol,
                            int max_parallel_downloads);

/* Push a fresh probability array into self->p_map */
RET request_handler_v2_update_pmap(request_handler_v2_t *self,
                                   const float *p_map_src);

static bool is_tile_in_actual_viewport(int tile_id, float actual_yaw, float actual_pitch);

/* chosen_versions[] is used only when self->cts_out == NULL */
RET request_handler_v2_post_get_info(request_handler_v2_t *self,
                                     COUNT chunk_id,
                                     int *vp_tiles,
                                     int num_vp_tiles,
                                     int *chosen_versions,
                                     float actual_yaw,
                                     float actual_pitch,
                                     HTTP_VERSION protocol,
                                     resource_monitor_t *rm);

RET request_handler_v2_post_get_info_no_rm(request_handler_v2_t *self,
                                           COUNT chunk_id,
                                           int *vp_tiles,
                                           int num_vp_tiles,
                                           int *chosen_versions,
                                           float actual_yaw,
                                           float actual_pitch,
                                           HTTP_VERSION protocol);

RET request_handler_v2_reset(request_handler_v2_t *self);
RET request_handler_v2_destroy(request_handler_v2_t *self);
int tile_version_to_num(int version);

/* Adaptive two-stage dispatch. Passing rm enables the PSI/SLR-μ gate. */
RET request_handler_v2_post_get_info_two_stage(
        request_handler_v2_t *self,
        COUNT chunk_id,
        int  *core_tiles,   int n_core,
        int  *vp_tiles,     int num_vp_tiles,
        int  *chosen_versions,
        float actual_yaw,   float actual_pitch,
        HTTP_VERSION protocol,
        resource_monitor_t *rm);

#endif /* REQUEST_HANDLER_V2_H */
