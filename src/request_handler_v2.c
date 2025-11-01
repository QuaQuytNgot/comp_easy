#include "proto_comp/request_handler_v2.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// RET request_handler_v2_init(request_handler_v2_t *self,
//                             const char *ser_adrr,
//                             COUNT seg_count,
//                             COUNT version_count,
//                             COUNT tile_count,
//                             HTTP_VERSION protocol,
//                             int max_parallel_downloads)
// {
//     if (!self || !ser_addr) return RET_FAIL;
    
//     memset(self, 0, sizeof(request_handler_v2_t));
    
//     self->ser_addr = (char *)ser_addr;
//     self->seg_count = seg_count;
//     self->tile_count = tile_count;
//     self->version_count = version_count;
//     self->max_parallel_downloads = max_parallel;
    
//     /* Initialize HTTP connection pool */
//     if (http_pool_init(&self->http_pool, http_ver) != RET_SUCCESS) {
//         return RET_FAIL;
//     }
    
//     /* Allocate arrays */
//     self->data = (buffer_t *)calloc(tile_count, sizeof(buffer_t));
//     if (!self->data) goto cleanup;
    
//     for (COUNT i = 0; i < tile_count; i++) {
//         if (buffer_init(&self->data[i]) != RET_SUCCESS) {
//             for (COUNT j = 0; j < i; j++) {
//                 buffer_destroy(&self->data[j]);
//             }
//             goto cleanup;
//         }
//     }
    
//     self->dls = (bw_t *)calloc(tile_count, sizeof(bw_t));
//     self->cnnt = (count_time_t *)calloc(tile_count, sizeof(count_time_t));
//     self->pre_trans_time = (count_time_t *)calloc(tile_count, sizeof(count_time_t));
//     self->start_trans_time = (count_time_t *)calloc(tile_count, sizeof(count_time_t));
//     self->total_time = (count_time_t *)calloc(tile_count, sizeof(count_time_t));
//     self->size_dl = (count_time_t *)calloc(tile_count, sizeof(count_time_t));
    
//     if (!self->dls || !self->cnnt || !self->pre_trans_time || 
//         !self->start_trans_time || !self->total_time || !self->size_dl) {
//         goto cleanup;
//     }
    
//     return RET_SUCCESS;
    
// cleanup:
//     request_handler_v2_destroy(self);
//     return RET_FAIL;
// }

RET request_handler_v2_init(request_handler_v2_t *self,
                            const char *ser_adrr,
                            COUNT seg_count,
                            COUNT version_count,
                            COUNT tile_count,
                            HTTP_VERSION protocol,
                            int max_parallel_downloads)
{
  self->ser_addr = (char *)ser_adrr;
  self->seg_count = seg_count;
  self->tile_count = tile_count;
  self->version_count = version_count;

  // 1. Khởi tạo pool
  self->pool = (http_pool_t *)malloc(sizeof(http_pool_t));
  if (self->pool == NULL)
  {
    return RET_FAIL;
  }
  if (http_pool_init(self->pool, protocol, max_parallel_downloads) != RET_SUCCESS)
  {
    free(self->pool);
    return RET_FAIL;
  }

  // 2. Khởi tạo các buffer dữ liệu (giống v1)
  self->data = (buffer_t *)malloc(tile_count * sizeof(buffer_t));
  if (self->data == NULL)
  {
    http_pool_destroy(self->pool);
    free(self->pool);
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
      http_pool_destroy(self->pool);
      free(self->pool);
      return RET_FAIL;
    }
  }

  return RET_SUCCESS;
}

static void calculate_group_stats_v2(download_job_t *job_group,
                                     int num_tiles_in_group,
                                     tile_group_stats_t *stats)
{
    memset(stats, 0, sizeof(tile_group_stats_t));
    
    stats->min_download_speed = UINT64_MAX;
    stats->max_download_speed = 0;
    
    count_time_t *timing_array = NULL;
    if (num_tiles_in_group > 1) {
        timing_array = (count_time_t *)malloc(num_tiles_in_group * sizeof(count_time_t));
    }
    
    for (int i = 0; i < num_tiles_in_group; i++) {
        download_job_t *job = &job_group[i];
        
        // Chỉ tính toán nếu request thành công
        if (job->status == JOB_STATUS_COMPLETED && job->metrics.size_download > 0) {
            
            http_metrics_t *metrics = &job->metrics;

            // Basic metrics
            stats->total_connect_time += metrics->connect_time;
            stats->total_pretransfer_time += metrics->pretransfer_time;
            stats->total_starttransfer_time += metrics->starttransfer_time;
            stats->total_time += metrics->total_time;
            stats->avg_download_speed += metrics->speed_download;
            stats->total_size += metrics->size_download;
            
            // Min/Max speed
            if (metrics->speed_download < stats->min_download_speed) {
                stats->min_download_speed = metrics->speed_download;
            }
            if (metrics->speed_download > stats->max_download_speed) {
                stats->max_download_speed = metrics->speed_download;
            }
            
            // NEW: Advanced metrics
            stats->avg_namelookup_time += metrics->namelookup_time;
            stats->avg_appconnect_time += metrics->appconnect_time;
            stats->num_connections_reused += metrics->connection_reused;
            
            // Store timing for jitter calculation
            if (timing_array) {
                timing_array[stats->tile_count] = metrics->total_time;
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
            double mean = (double)stats->avg_total_time; // Chuyển sang double để tính toán
            
            for (int i = 0; i < stats->tile_count; i++) {
                double diff = (double)timing_array[i] - mean;
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

RET request_handler_v2_post_get_info(request_handler_v2_t *self,
                                     COUNT chunk_id,
                                     int *vp_tiles,
                                     int num_vp_tiles,
                                     int *chosen_versions) {
    if (!self) return RET_FAIL;
    
    printf("=== CHUNK %lld DOWNLOAD STARTED ===\n", chunk_id + 1);
    
    /* Prepare download tasks for viewport tiles */
    download_task_t *vp_tasks = (download_task_t *)calloc(num_vp_tiles, 
                                                          sizeof(download_task_t));
    if (!vp_tasks) return RET_FAIL;
    
    char **url_buffers = (char **)calloc(num_vp_tiles, sizeof(char *));
    if (!url_buffers) {
        free(vp_tasks);
        return RET_FAIL;
    }
    
    /* Build URLs for viewport tiles */
    for (int i = 0; i < num_vp_tiles; i++) {
        int tile_id = vp_tiles[i];
        int version = chosen_versions[i];
        int qp = tile_version_to_num(version);
        
        url_buffers[i] = (char *)malloc(512);
        if (!url_buffers[i]) {
            for (int j = 0; j < i; j++) free(url_buffers[j]);
            free(url_buffers);
            free(vp_tasks);
            return RET_FAIL;
        }
        
        snprintf(url_buffers[i], 512,
                "%s/beach/beach_%lld/erp_8x6/tile_yuv/tile_%d_%d_480x360_QP%d.bin",
                self->ser_addr, chunk_id + 1,
                tile_id / NO_OF_COLS, tile_id % NO_OF_COLS, qp);
        
        vp_tasks[i].url = url_buffers[i];
        vp_tasks[i].tile_id = tile_id;
        vp_tasks[i].data = &self->data[tile_id];
        vp_tasks[i].dls = &self->dls[tile_id];
        vp_tasks[i].cnnt = &self->cnnt[tile_id];
        vp_tasks[i].pre_trans_time = &self->pre_trans_time[tile_id];
        vp_tasks[i].start_trans_time = &self->start_trans_time[tile_id];
        vp_tasks[i].total_time = &self->total_time[tile_id];
        vp_tasks[i].size_dl = &self->size_dl[tile_id];
    }
    
    /* Download viewport tiles in parallel */
    printf("\n[VIEWPORT] Downloading %d tiles in parallel (max %d concurrent)...\n",
           num_vp_tiles, self->max_parallel_downloads);
    
    RET ret = http_pool_get_parallel(&self->http_pool, vp_tasks, num_vp_tiles,
                                     self->max_parallel_downloads);
    
    /* Print results */
    for (int i = 0; i < num_vp_tiles; i++) {
        int tile_id = vp_tiles[i];
        printf("\n[VIEWPORT] Tile %d (row %d, col %d) - Version %d (QP%d)\n",
               tile_id, tile_id / NO_OF_COLS, tile_id % NO_OF_COLS,
               chosen_versions[i], tile_version_to_num(chosen_versions[i]));
        printf("  Buffer size    : %zu bytes\n", self->data[tile_id].size);
        printf("  Download speed : %ld bps (%.2f Mbps)\n",
               self->dls[tile_id], self->dls[tile_id] / 1000000.0);
        printf("  Total time     : %lld us (%.3f ms)\n",
               self->total_time[tile_id], self->total_time[tile_id] / 1000.0);
        printf("  Status         : %s\n",
               (vp_tasks[i].status == RET_SUCCESS) ? "SUCCESS" : "FAILED");
    }
    
    /* Cleanup viewport tasks */
    for (int i = 0; i < num_vp_tiles; i++) {
        free(url_buffers[i]);
    }
    free(url_buffers);
    free(vp_tasks);
    
    /* Download non-viewport tiles in parallel */
    int non_vp_count = 0;
    for (COUNT i = 0; i < self->tile_count; i++) {
        bool is_vp = false;
        for (int j = 0; j < num_vp_tiles; j++) {
            if (vp_tiles[j] == i) {
                is_vp = true;
                break;
            }
        }
        if (!is_vp) non_vp_count++;
    }
    
    if (non_vp_count > 0) {
        printf("\n=== NON-VIEWPORT TILES ===\n");
        printf("[NON-VP] Downloading %d tiles in parallel...\n", non_vp_count);
        
        download_task_t *non_vp_tasks = (download_task_t *)calloc(non_vp_count,
                                                                  sizeof(download_task_t));
        char **non_vp_urls = (char **)calloc(non_vp_count, sizeof(char *));
        
        if (non_vp_tasks && non_vp_urls) {
            int task_idx = 0;
            for (COUNT tile_id = 0; tile_id < self->tile_count; tile_id++) {
                bool is_vp = false;
                for (int j = 0; j < num_vp_tiles; j++) {
                    if (vp_tiles[j] == tile_id) {
                        is_vp = true;
                        break;
                    }
                }
                
                if (!is_vp) {
                    non_vp_urls[task_idx] = (char *)malloc(512);
                    snprintf(non_vp_urls[task_idx], 512,
                            "%s/beach/beach_%lld/erp_8x6/tile_yuv/tile_%lld_%lld_480x360_QP38.bin",
                            self->ser_addr, chunk_id + 1,
                            tile_id / NO_OF_COLS, tile_id % NO_OF_COLS);
                    
                    non_vp_tasks[task_idx].url = non_vp_urls[task_idx];
                    non_vp_tasks[task_idx].tile_id = tile_id;
                    non_vp_tasks[task_idx].data = &self->data[tile_id];
                    non_vp_tasks[task_idx].dls = &self->dls[tile_id];
                    non_vp_tasks[task_idx].cnnt = &self->cnnt[tile_id];
                    non_vp_tasks[task_idx].pre_trans_time = &self->pre_trans_time[tile_id];
                    non_vp_tasks[task_idx].start_trans_time = &self->start_trans_time[tile_id];
                    non_vp_tasks[task_idx].total_time = &self->total_time[tile_id];
                    non_vp_tasks[task_idx].size_dl = &self->size_dl[tile_id];
                    task_idx++;
                }
            }
            
            http_pool_get_parallel(&self->http_pool, non_vp_tasks, non_vp_count,
                                  self->max_parallel_downloads);
            
            /* Cleanup */
            for (int i = 0; i < non_vp_count; i++) {
                free(non_vp_urls[i]);
            }
        }
        
        free(non_vp_urls);
        free(non_vp_tasks);
    }
    
    /* Print summary */
    printf("\n=== CHUNK %lld SUMMARY ===\n", chunk_id);
    int64_t total_bytes = 0, total_time_sum = 0;
    int successful_tiles = 0;
    
    for (COUNT i = 0; i < self->tile_count; i++) {
        if (self->size_dl[i] > 0) {
            total_bytes += self->size_dl[i];
            total_time_sum += self->total_time[i];
            successful_tiles++;
        }
    }
    
    if (successful_tiles > 0) {
        printf("Total tiles downloaded: %d\n", successful_tiles);
        printf("Total data size: %ld bytes (%.2f MB)\n",
               total_bytes, total_bytes / 1048576.0);
        printf("Average download time: %.3f ms\n",
               (total_time_sum / successful_tiles) / 1000.0);
        printf("Effective throughput: %.2f Mbps\n",
               (total_bytes * 8.0) / (total_time_sum / 1000000.0) / 1000000.0);
    }
    
    printf("=== CHUNK %lld COMPLETED ===\n\n", chunk_id);
    
    return ret;
}

RET request_handler_v2_reset(request_handler_v2_t *self) {
    if (!self) return RET_FAIL;
    
    for (COUNT i = 0; i < self->tile_count; i++) {
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

RET request_handler_v2_destroy(request_handler_v2_t *self) {
    if (!self) return RET_FAIL;
    
    http_pool_destroy(&self->http_pool);
    
    if (self->data) {
        for (COUNT i = 0; i < self->tile_count; i++) {
            buffer_destroy(&self->data[i]);
        }
        free(self->data);
    }
    
    free(self->dls);
    free(self->cnnt);
    free(self->pre_trans_time);
    free(self->start_trans_time);
    free(self->total_time);
    free(self->size_dl);
    
    memset(self, 0, sizeof(request_handler_v2_t));
    return RET_SUCCESS;
}