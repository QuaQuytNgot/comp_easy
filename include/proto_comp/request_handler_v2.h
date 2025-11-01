#ifndef REQUEST_HANDLER_V2_H
#define REQUEST_HANDLER_V2_H

#include "buffer.h"
#include "define.h"
#include "http_pool.h"

typedef struct
{
  int tile_count;

  // Timing metrics (microseconds)
  count_time_t total_connect_time;
  count_time_t total_pretransfer_time;
  count_time_t total_starttransfer_time;
  count_time_t total_time;
  count_time_t avg_connect_time;
  count_time_t avg_pretransfer_time;
  count_time_t avg_starttransfer_time;
  count_time_t avg_total_time;

  // Speed metrics
  bw_t avg_download_speed;
  bw_t min_download_speed;
  bw_t max_download_speed;

  // Size metrics
  size_t total_size;
  size_t avg_size;

  // NEW: Advanced metrics
  count_time_t avg_namelookup_time; // DNS resolution time
  count_time_t avg_appconnect_time; // SSL/TLS handshake time
  int num_connections_reused;       // Connection reuse count
  double jitter_ms;                 // Timing variation (for non-VP)

} tile_group_stats_t;

typedef struct request_handler_v2_t {
    char *ser_addr;
    COUNT seg_count;
    COUNT tile_count;
    COUNT version_count;
    
    buffer_t *data;
    bw_t *dls;
    count_time_t *cnnt;
    count_time_t *pre_trans_time;
    count_time_t *start_trans_time;
    count_time_t *total_time;
    count_time_t *size_dl;
    
    http_pool_t http_pool;  // Connection pool
    int max_parallel_downloads;  // Max concurrent downloads
} request_handler_v2_t;

static void calculate_group_stats_v2(download_job_t *job_group,
                                     int num_tiles_in_group,
                                     tile_group_stats_t *stats);

RET request_handler_v2_init(request_handler_v2_t *self,
                            const char *ser_adrr,
                            COUNT seg_count,
                            COUNT version_count,
                            COUNT tile_count,
                            HTTP_VERSION protocol,
                            int max_parallel_downloads);

RET request_handler_v2_download_segment(request_handler_v2_t *self,
                                        COUNT chunk_id,
                                        int *vp_tiles,
                                        int num_vp_tiles,
                                        int *chosen_versions,
                                        HTTP_VERSION protocol);

RET request_handler_v2_reset(request_handler_v2_t *self);
RET request_handler_v2_destroy(request_handler_v2_t *self);

int tile_version_to_num(int version);
#endif