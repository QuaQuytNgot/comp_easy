#include <proto_comp/http.h>
#include <proto_comp/request_handler.h>
#include <string.h>

RET request_handler_init(request_handler_t *self,
                         const char *ser_adrr,
                         COUNT seg_count,
                         COUNT version_count,
                         COUNT tile_count)
{
  self->ser_addr = ser_adrr;
  self->seg_count = seg_count;
  self->tile_count = tile_count;
  self->version_count = version_count;

  self->data = (buffer_t *)malloc(tile_count * sizeof(buffer_t));
  self->namelookup_time = (count_time_t *)calloc(tile_count, sizeof(count_time_t));
  if (self->namelookup_time == NULL)
  {
    return RET_FAIL;
  }

  self->appconnect_time = (count_time_t *)calloc(tile_count, sizeof(count_time_t));
  if (self->appconnect_time == NULL)
  {
    free(self->namelookup_time);
    return RET_FAIL;
  }

  self->connection_reused = (int *)calloc(tile_count, sizeof(int));
  if (self->connection_reused == NULL)
  {
    free(self->appconnect_time);
    free(self->namelookup_time);
    return RET_FAIL;
  }

  if (self->data == NULL)
  {
    return RET_FAIL;
  }

  for (COUNT i = 0; i < tile_count; i++)
  {
    if (buffer_init(&self->data[i]) != RET_SUCCESS)
    {
      for (COUNT j = 0; j < i; j++)
      {
        buffer_destroy(&self->data[j]);
      }
      free(self->data);
      return RET_FAIL;
    }
  }

  self->dls = (bw_t *)malloc(tile_count * sizeof(bw_t));
  if (self->dls == NULL)
  {
    for (COUNT i = 0; i < tile_count; i++)
    {
      buffer_destroy(&self->data[i]);
    }
    free(self->data);
    return RET_FAIL;
  }

  self->cnnt =
      (count_time_t *)malloc(tile_count * sizeof(count_time_t));
  if (self->cnnt == NULL)
  {
    free(self->dls);
    for (COUNT i = 0; i < tile_count; i++)
    {
      buffer_destroy(&self->data[i]);
    }
    free(self->data);
    return RET_FAIL;
  }

  self->pre_trans_time =
      (count_time_t *)malloc(tile_count * sizeof(count_time_t));
  if (self->pre_trans_time == NULL)
  {
    free(self->dls);
    free(self->cnnt);
    for (COUNT i = 0; i < tile_count; i++)
    {
      buffer_destroy(&self->data[i]);
    }
    free(self->data);
    return RET_FAIL;
  }

  self->start_trans_time =
      (count_time_t *)malloc(tile_count * sizeof(count_time_t));
  if (self->start_trans_time == NULL)
  {
    free(self->dls);
    free(self->cnnt);
    free(self->pre_trans_time);
    for (COUNT i = 0; i < tile_count; i++)
    {
      buffer_destroy(&self->data[i]);
    }
    free(self->data);
    return RET_FAIL;
  }

  self->total_time =
      (count_time_t *)malloc(tile_count * sizeof(count_time_t));
  if (self->total_time == NULL)
  {
    free(self->dls);
    free(self->cnnt);
    free(self->pre_trans_time);
    free(self->start_trans_time);
    for (COUNT i = 0; i < tile_count; i++)
    {
      buffer_destroy(&self->data[i]);
    }
    free(self->data);
    return RET_FAIL;
  }

  self->size_dl =
      (count_time_t *)malloc(tile_count * sizeof(count_time_t));
  if (self->size_dl == NULL)
  {
    free(self->dls);
    free(self->cnnt);
    free(self->pre_trans_time);
    free(self->start_trans_time);
    free(self->total_time);
    for (COUNT i = 0; i < tile_count; i++)
    {
      buffer_destroy(&self->data[i]);
    }
    free(self->data);
    return RET_FAIL;
  }

  for (COUNT i = 0; i < tile_count; i++)
  {
    self->dls[i] = 0;
    self->cnnt[i] = 0;
    self->pre_trans_time[i] = 0;
    self->start_trans_time[i] = 0;
    self->total_time[i] = 0;
    self->size_dl[i] = 0;
  }

  return RET_SUCCESS;
}

static void calculate_group_stats(request_handler_t *self,
                                  int *tile_ids,
                                  int num_tiles,
                                  tile_group_stats_t *stats)
{
    memset(stats, 0, sizeof(tile_group_stats_t));
    
    stats->min_download_speed = UINT64_MAX;
    stats->max_download_speed = 0;
    
    count_time_t *timing_array = NULL;
    if (num_tiles > 1) {
        timing_array = (count_time_t *)malloc(num_tiles * sizeof(count_time_t));
    }
    
    for (int i = 0; i < num_tiles; i++) {
        int tile_id = tile_ids[i];
        if (self->size_dl[tile_id] > 0) {
            // Basic metrics
            stats->total_connect_time += self->cnnt[tile_id];
            stats->total_pretransfer_time += self->pre_trans_time[tile_id];
            stats->total_starttransfer_time += self->start_trans_time[tile_id];
            stats->total_time += self->total_time[tile_id];
            stats->avg_download_speed += self->dls[tile_id];
            stats->total_size += self->size_dl[tile_id];
            
            // Min/Max speed
            if (self->dls[tile_id] < stats->min_download_speed) {
                stats->min_download_speed = self->dls[tile_id];
            }
            if (self->dls[tile_id] > stats->max_download_speed) {
                stats->max_download_speed = self->dls[tile_id];
            }
            
            // NEW: Advanced metrics
            stats->avg_namelookup_time += self->namelookup_time[tile_id];
            stats->avg_appconnect_time += self->appconnect_time[tile_id];
            stats->num_connections_reused += self->connection_reused[tile_id];
            
            // Store timing for jitter calculation
            if (timing_array) {
                timing_array[stats->tile_count] = self->total_time[tile_id];
            }
            
            stats->tile_count++;
        }
    }
    
    if (stats->tile_count > 0) {
        // Calculate averages
        stats->avg_connect_time = stats->total_connect_time / stats->tile_count;
        stats->avg_pretransfer_time = stats->total_pretransfer_time / stats->tile_count;
        stats->avg_starttransfer_time = stats->total_starttransfer_time / stats->tile_count;
        stats->avg_total_time = stats->total_time / stats->tile_count;
        stats->avg_download_speed = stats->avg_download_speed / stats->tile_count;
        stats->avg_size = stats->total_size / stats->tile_count;
        stats->avg_namelookup_time = stats->avg_namelookup_time / stats->tile_count;
        stats->avg_appconnect_time = stats->avg_appconnect_time / stats->tile_count;
        
        // Calculate jitter (standard deviation of timing)
        if (stats->tile_count > 1 && timing_array) {
            double variance = 0.0;
            double mean = stats->avg_total_time;
            
            for (int i = 0; i < stats->tile_count; i++) {
                double diff = timing_array[i] - mean;
                variance += diff * diff;
            }
            variance /= stats->tile_count;
            stats->jitter_ms = sqrt(variance) / 1000.0; // Convert to ms
        }
    }
    
    if (timing_array) {
        free(timing_array);
    }
}


RET request_handler_post_get_info(request_handler_t *self,
                                  COUNT              chunk_id,
                                  int               *vp_tiles,
                                  int                num_vp_tiles,
                                  int               *chosen_versions,
                                  HTTP_VERSION       protocol)  // NEW: specify protocol
{
    if (self == NULL) {
        return RET_FAIL;
    }

    char url_buffer[512];
    const char *protocol_name = (protocol == STREAM_HTTP_2_0) ? "HTTP/2" : "HTTP/3";
    
    printf("\n========== SEGMENT %lld (%s) ==========\n", chunk_id + 1, protocol_name);

    // ===== VIEWPORT TILES DOWNLOAD =====
    printf("\n[VIEWPORT] Downloading %d tiles via %s...\n", num_vp_tiles, protocol_name);
    
    for (int i = 0; i < num_vp_tiles; i++) {
        int tile_id = vp_tiles[i];
        int version = chosen_versions[i];
        int qp = tile_version_to_num(version);
        
        snprintf(url_buffer, sizeof(url_buffer),
                "%s/beach/beach_%lld/erp_8x6/tile_yuv/tile_%d_%d_480x360_QP%d.bin",
                self->ser_addr, chunk_id + 1,
                tile_id / NO_OF_COLS, tile_id % NO_OF_COLS, qp);
        
        RET r = http_get_to_buffer(url_buffer, protocol,
                                   &self->data[tile_id],
                                   &self->dls[tile_id],
                                   &self->cnnt[tile_id],
                                   &self->pre_trans_time[tile_id],
                                   &self->start_trans_time[tile_id],
                                   &self->total_time[tile_id],
                                   &self->size_dl[tile_id],
                                   &self->namelookup_time[tile_id],
                                   &self->appconnect_time[tile_id],
                                   &self->connection_reused[tile_id]);
        
        if (r == RET_FAIL) {
            printf("  ERROR: Tile %d failed\n", tile_id);
        }
    }
    
    // Calculate VP stats
    tile_group_stats_t vp_stats;
    calculate_group_stats(self, vp_tiles, num_vp_tiles, &vp_stats);
    
    printf("\n[VIEWPORT SUMMARY]\n");
    printf("  Protocol         : %s\n", protocol_name);
    printf("  Tiles            : %d\n", vp_stats.tile_count);
    printf("  Total Size       : %.2f MB\n", vp_stats.total_size / 1048576.0);
    printf("  Avg Tile Size    : %.2f KB\n", vp_stats.avg_size / 1024.0);
    printf("\n  Speed Statistics:\n");
    printf("    Average        : %.2f Mbps\n", vp_stats.avg_download_speed / 1000000.0);
    printf("    Min            : %.2f Mbps\n", vp_stats.min_download_speed / 1000000.0);
    printf("    Max            : %.2f Mbps\n", vp_stats.max_download_speed / 1000000.0);
    printf("\n  Connection Metrics:\n");
    printf("    Connections Reused: %d / %d (%.1f%%)\n", 
           vp_stats.num_connections_reused, vp_stats.tile_count,
           (vp_stats.tile_count > 0) ? (vp_stats.num_connections_reused * 100.0 / vp_stats.tile_count) : 0);
    printf("\n  Timing Breakdown (avg):\n");
    printf("    DNS Lookup     : %.2f ms\n", vp_stats.avg_namelookup_time / 1000.0);
    printf("    TCP Connect    : %.2f ms\n", 
           (vp_stats.avg_connect_time - vp_stats.avg_namelookup_time) / 1000.0);
    printf("    TLS Handshake  : %.2f ms\n", 
           (vp_stats.avg_appconnect_time - vp_stats.avg_connect_time) / 1000.0);
    printf("    Request Sent   : %.2f ms\n", 
           (vp_stats.avg_pretransfer_time - vp_stats.avg_appconnect_time) / 1000.0);
    printf("    TTFB           : %.2f ms\n", 
           (vp_stats.avg_starttransfer_time - vp_stats.avg_pretransfer_time) / 1000.0);
    printf("    Data Transfer  : %.2f ms\n", 
           (vp_stats.avg_total_time - vp_stats.avg_starttransfer_time) / 1000.0);
    printf("    Total (avg)    : %.2f ms\n", vp_stats.avg_total_time / 1000.0);
    printf("\n  Parallel Download:\n");
    printf("    Total Time     : %.2f ms\n", vp_stats.total_time / 1000.0);
    printf("    Jitter (σ)     : %.2f ms\n", vp_stats.jitter_ms);
    
    // ===== NON-VIEWPORT TILES =====
    int non_vp_tiles[NO_OF_ROWS * NO_OF_COLS];
    int non_vp_count = 0;
    
    for (COUNT tile_id = 0; tile_id < self->tile_count; tile_id++) {
        bool is_viewport_tile = false;
        for (int i = 0; i < num_vp_tiles; i++) {
            if (vp_tiles[i] == tile_id) {
                is_viewport_tile = true;
                break;
            }
        }
        if (!is_viewport_tile) {
            non_vp_tiles[non_vp_count++] = tile_id;
        }
    }
    
    printf("\n[NON-VIEWPORT] Downloading %d tiles via %s...\n", non_vp_count, protocol_name);
    
    for (int i = 0; i < non_vp_count; i++) {
        COUNT tile_id = non_vp_tiles[i];
        
        snprintf(url_buffer, sizeof(url_buffer),
                "%s/beach/beach_%lld/erp_8x6/tile_yuv/tile_%lld_%lld_480x360_QP38.bin",
                self->ser_addr, chunk_id + 1,
                tile_id / NO_OF_COLS, tile_id % NO_OF_COLS);
        
        RET r = http_get_to_buffer(url_buffer, protocol,
                                   &self->data[tile_id],
                                   &self->dls[tile_id],
                                   &self->cnnt[tile_id],
                                   &self->pre_trans_time[tile_id],
                                   &self->start_trans_time[tile_id],
                                   &self->total_time[tile_id],
                                   &self->size_dl[tile_id],
                                   &self->namelookup_time[tile_id],
                                   &self->appconnect_time[tile_id],
                                   &self->connection_reused[tile_id]);
        
        if (r == RET_FAIL) {
            printf("  ERROR: Tile %lld failed\n", tile_id);
        }
    }
    
    // Calculate Non-VP stats
    tile_group_stats_t non_vp_stats;
    calculate_group_stats(self, non_vp_tiles, non_vp_count, &non_vp_stats);
    
    printf("\n[NON-VIEWPORT SUMMARY]\n");
    printf("  Protocol         : %s\n", protocol_name);
    printf("  Tiles            : %d\n", non_vp_stats.tile_count);
    printf("  Total Size       : %.2f MB\n", non_vp_stats.total_size / 1048576.0);
    printf("  Avg Tile Size    : %.2f KB\n", non_vp_stats.avg_size / 1024.0);
    printf("\n  Speed Statistics:\n");
    printf("    Average        : %.2f Mbps\n", non_vp_stats.avg_download_speed / 1000000.0);
    printf("    Min            : %.2f Mbps\n", non_vp_stats.min_download_speed / 1000000.0);
    printf("    Max            : %.2f Mbps\n", non_vp_stats.max_download_speed / 1000000.0);
    printf("\n  Connection Metrics:\n");
    printf("    Connections Reused: %d / %d (%.1f%%)\n", 
           non_vp_stats.num_connections_reused, non_vp_stats.tile_count,
           (non_vp_stats.tile_count > 0) ? (non_vp_stats.num_connections_reused * 100.0 / non_vp_stats.tile_count) : 0);
    printf("\n  Timing Breakdown (avg):\n");
    printf("    DNS Lookup     : %.2f ms\n", non_vp_stats.avg_namelookup_time / 1000.0);
    printf("    TCP Connect    : %.2f ms\n", 
           (non_vp_stats.avg_connect_time - non_vp_stats.avg_namelookup_time) / 1000.0);
    printf("    TLS Handshake  : %.2f ms\n", 
           (non_vp_stats.avg_appconnect_time - non_vp_stats.avg_connect_time) / 1000.0);
    printf("    Request Sent   : %.2f ms\n", 
           (non_vp_stats.avg_pretransfer_time - non_vp_stats.avg_appconnect_time) / 1000.0);
    printf("    TTFB           : %.2f ms\n", 
           (non_vp_stats.avg_starttransfer_time - non_vp_stats.avg_pretransfer_time) / 1000.0);
    printf("    Data Transfer  : %.2f ms\n", 
           (non_vp_stats.avg_total_time - non_vp_stats.avg_starttransfer_time) / 1000.0);
    printf("    Total (avg)    : %.2f ms\n", non_vp_stats.avg_total_time / 1000.0);
    printf("\n  Parallel Download:\n");
    printf("    Total Time     : %.2f ms\n", non_vp_stats.total_time / 1000.0);
    printf("    Jitter (σ)     : %.2f ms\n", non_vp_stats.jitter_ms);
    
    // ===== SEGMENT TOTAL =====
    int64_t total_bytes = vp_stats.total_size + non_vp_stats.total_size;
    count_time_t total_time = vp_stats.total_time + non_vp_stats.total_time;
    int total_tiles = vp_stats.tile_count + non_vp_stats.tile_count;
    int total_reused = vp_stats.num_connections_reused + non_vp_stats.num_connections_reused;
    
    printf("\n[SEGMENT %lld OVERALL]\n", chunk_id + 1);
    printf("  Protocol         : %s\n", protocol_name);
    printf("  Total Tiles      : %d (VP: %d, Non-VP: %d)\n", 
           total_tiles, vp_stats.tile_count, non_vp_stats.tile_count);
    printf("  Total Size       : %.2f MB\n", total_bytes / 1048576.0);
    printf("  Total Time       : %.2f ms\n", total_time / 1000.0);
    printf("  Throughput       : %.2f Mbps\n", 
           (total_bytes * 8.0) / (total_time / 1000000.0) / 1000000.0);
    printf("  Conn Reuse Rate  : %.1f%% (%d/%d)\n",
           (total_tiles > 0) ? (total_reused * 100.0 / total_tiles) : 0,
           total_reused, total_tiles);
    printf("===============================\n\n");
    
    // ===== CSV EXPORT =====
    FILE *csv_file = fopen("http_metrics.csv", "a");
    if (csv_file) {
        fseek(csv_file, 0, SEEK_END);
        if (ftell(csv_file) == 0) {
            fprintf(csv_file, "segment_id,protocol,tile_type,num_tiles,"
                   "total_size_mb,avg_size_kb,"
                   "avg_speed_mbps,min_speed_mbps,max_speed_mbps,"
                   "dns_ms,tcp_connect_ms,tls_handshake_ms,ttfb_ms,transfer_ms,total_ms,"
                   "parallel_total_ms,jitter_ms,conn_reused,conn_reuse_pct\n");
        }
        
        // VP row
        fprintf(csv_file, "%lld,%s,viewport,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%.1f\n",
                chunk_id + 1, protocol_name, vp_stats.tile_count,
                vp_stats.total_size / 1048576.0, vp_stats.avg_size / 1024.0,
                vp_stats.avg_download_speed / 1000000.0,
                vp_stats.min_download_speed / 1000000.0,
                vp_stats.max_download_speed / 1000000.0,
                vp_stats.avg_namelookup_time / 1000.0,
                (vp_stats.avg_connect_time - vp_stats.avg_namelookup_time) / 1000.0,
                (vp_stats.avg_appconnect_time - vp_stats.avg_connect_time) / 1000.0,
                (vp_stats.avg_starttransfer_time - vp_stats.avg_pretransfer_time) / 1000.0,
                (vp_stats.avg_total_time - vp_stats.avg_starttransfer_time) / 1000.0,
                vp_stats.avg_total_time / 1000.0,
                vp_stats.total_time / 1000.0, vp_stats.jitter_ms,
                vp_stats.num_connections_reused,
                (vp_stats.tile_count > 0) ? (vp_stats.num_connections_reused * 100.0 / vp_stats.tile_count) : 0);
        
        // Non-VP row
        fprintf(csv_file, "%lld,%s,non-viewport,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%.1f\n",
                chunk_id + 1, protocol_name, non_vp_stats.tile_count,
                non_vp_stats.total_size / 1048576.0, non_vp_stats.avg_size / 1024.0,
                non_vp_stats.avg_download_speed / 1000000.0,
                non_vp_stats.min_download_speed / 1000000.0,
                non_vp_stats.max_download_speed / 1000000.0,
                non_vp_stats.avg_namelookup_time / 1000.0,
                (non_vp_stats.avg_connect_time - non_vp_stats.avg_namelookup_time) / 1000.0,
                (non_vp_stats.avg_appconnect_time - non_vp_stats.avg_connect_time) / 1000.0,
                (non_vp_stats.avg_starttransfer_time - non_vp_stats.avg_pretransfer_time) / 1000.0,
                (non_vp_stats.avg_total_time - non_vp_stats.avg_starttransfer_time) / 1000.0,
                non_vp_stats.avg_total_time / 1000.0,
                non_vp_stats.total_time / 1000.0, non_vp_stats.jitter_ms,
                non_vp_stats.num_connections_reused,
                (non_vp_stats.tile_count > 0) ? (non_vp_stats.num_connections_reused * 100.0 / non_vp_stats.tile_count) : 0);
        
        fclose(csv_file);
    }
    
    return RET_SUCCESS;
}
RET request_handler_reset(request_handler_t *self)
{
  if (self == NULL)
  {
    return RET_FAIL;
  }

  for (COUNT i = 0; i < self->tile_count; i++)
  {
    buffer_destroy(&self->data[i]);
    buffer_init(&self->data[i]);

    self->dls[i] = 0;
    self->cnnt[i] = 0;
    self->pre_trans_time[i] = 0;
    self->start_trans_time[i] = 0;
    self->total_time[i] = 0;
    self->size_dl[i] = 0;
  }
  return RET_SUCCESS;
}

RET request_handler_destroy(request_handler_t *self)
{
  if (self == NULL)
  {
    return RET_FAIL;
  }
  if (self->data != NULL)
  {
    for (COUNT i = 0; i < self->tile_count; i++)
    {
      buffer_destroy(&self->data[i]);
    }
    free(self->data);
  }

  if (self->dls != NULL)
    free(self->dls);

  if (self->cnnt != NULL)
    free(self->cnnt);

  if (self->pre_trans_time != NULL)
    free(self->pre_trans_time);

  if (self->start_trans_time != NULL)
    free(self->start_trans_time);

  if (self->total_time != NULL)
    free(self->total_time);

  if (self->size_dl != NULL)
    free(self->size_dl);

  memset(self, 0, sizeof(request_handler_t));

  return RET_SUCCESS;
}

int tile_version_to_num(int version)
{
  switch (version)
  {
  case 0:
    return 38;
    break;
  case 1:
    return 32;
    break;
  case 2:
    return 28;
    break;
  case 3:
    return 24;
    break;
  case 4:
    return 20;
    break;
  default:
    break;
  }
}