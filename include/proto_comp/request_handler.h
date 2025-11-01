#ifndef REQUEST_HANDLER_H
#define REQUEST_HANDLER_H

#include "buffer.h"
#include "define.h"

// typedef request_handler_t request_handler_t;

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

typedef struct request_handler_t
{
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
  count_time_t *namelookup_time;
  count_time_t *appconnect_time;
  int *connection_reused; // 0 = new, 1 = reused
} request_handler_t;

RET request_handler_init(request_handler_t *self,
                         const char *ser_adrr,
                         COUNT seg_count,
                         COUNT version_count,
                         COUNT tile_count);

RET request_handler_post_get_info(request_handler_t *self,
                                  COUNT chunk_id,
                                  int *vp_tiles,
                                  int num_vp_tiles,
                                  int *chosen_versions,
                                  HTTP_VERSION protocol);
// should add one more argument to take dl speed for bw_estimation
// same for time downloading
static void calculate_group_stats(request_handler_t *self,
                                  int *tile_ids,
                                  int num_tiles,
                                  tile_group_stats_t *stats);

RET request_handler_reset(request_handler_t *self);

RET request_handler_destroy(request_handler_t *self);

int tile_version_to_num(int version);

#endif
