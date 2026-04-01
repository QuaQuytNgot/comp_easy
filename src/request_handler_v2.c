// #include "proto_comp/request_handler_v2.h"
// #include <stdio.h>
// #include <string.h>
// #include <stdlib.h>

// RET request_handler_v2_init(request_handler_v2_t *self,
//                             const char *ser_adrr,
//                             COUNT seg_count,
//                             COUNT version_count,
//                             COUNT tile_count,
//                             HTTP_VERSION protocol,
//                             int max_parallel_downloads)
// {
//     if (!self || !ser_adrr) return RET_FAIL;
    
//     memset(self, 0, sizeof(request_handler_v2_t));
    
//     self->ser_addr = (char *)ser_adrr;
//     self->seg_count = seg_count;
//     self->tile_count = tile_count;
//     self->version_count = version_count;
//     self->max_parallel_downloads = max_parallel_downloads;
    
//     self->pool = (http_pool_t *)malloc(sizeof(http_pool_t));
//     if (self->pool == NULL) return RET_FAIL;
    
//     if (http_pool_init(self->pool, protocol, max_parallel_downloads) != RET_SUCCESS) {
//         free(self->pool);
//         return RET_FAIL;
//     }

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
//     self->namelookup_time = (count_time_t *)calloc(tile_count, sizeof(count_time_t));
//     self->appconnect_time = (count_time_t *)calloc(tile_count, sizeof(count_time_t));
//     self->redirect_time = (count_time_t *)calloc(tile_count, sizeof(count_time_t));
//     self->header_size = (size_t *)calloc(tile_count, sizeof(size_t));
//     self->redirect_count = (count_t *)calloc(tile_count, sizeof(int));
//     self->connection_reused = (int *)calloc(tile_count, sizeof(int));

//     if (!self->dls || !self->cnnt || !self->pre_trans_time || 
//         !self->start_trans_time || !self->total_time || !self->size_dl ||
//         !self->namelookup_time || !self->appconnect_time || 
//         !self->redirect_time || !self->header_size || 
//         !self->redirect_count || !self->connection_reused) {
//         goto cleanup;
//     }
    
//     return RET_SUCCESS;
    
// cleanup:
//     request_handler_v2_destroy(self);
//     return RET_FAIL;
// }

// static void calculate_group_stats(request_handler_v2_t *self,
//                                   int *tile_ids,
//                                   int num_tiles,
//                                   tile_group_stats_t *stats)
// {
//     memset(stats, 0, sizeof(tile_group_stats_t));
    
//     stats->min_download_speed = UINT64_MAX;
//     stats->max_download_speed = 0;
    
//     count_time_t *timing_array = NULL;
//     if (num_tiles > 1) {
//         timing_array = (count_time_t *)malloc(num_tiles * sizeof(count_time_t));
//     }
    
//     for (int i = 0; i < num_tiles; i++) {
//         int tile_id = tile_ids[i];
//         if (self->size_dl[tile_id] > 0) {

//             stats->total_namelookup_time += self->namelookup_time[tile_id];
//             stats->total_connect_time += self->cnnt[tile_id];
//             stats->total_appconnect_time += self->appconnect_time[tile_id];
//             stats->total_pretransfer_time += self->pre_trans_time[tile_id];
//             stats->total_starttransfer_time += self->start_trans_time[tile_id];
//             stats->total_time += self->total_time[tile_id];

//             stats->avg_download_speed += self->dls[tile_id];
//             if (self->dls[tile_id] < stats->min_download_speed) 
//                 stats->min_download_speed = self->dls[tile_id];
//             if (self->dls[tile_id] > stats->max_download_speed) 
//                 stats->max_download_speed = self->dls[tile_id];

//             stats->total_size += self->size_dl[tile_id];
//             stats->avg_header_size += self->header_size[tile_id];

//             stats->num_connections_reused += self->connection_reused[tile_id];
//             stats->total_redirects += self->redirect_count[tile_id];
//             stats->avg_redirect_time_ms += self->redirect_time[tile_id] / 1000.0;
            
//             if (timing_array) {
//                 timing_array[stats->tile_count] = self->total_time[tile_id];
//             }
            
//             stats->tile_count++;
//         }
//     }
    
//     if (stats->tile_count > 0) {
//         stats->avg_namelookup_time = stats->total_namelookup_time / stats->tile_count;
//         stats->avg_connect_time = stats->total_connect_time / stats->tile_count;
//         stats->avg_appconnect_time = stats->total_appconnect_time / stats->tile_count;
//         stats->avg_pretransfer_time = stats->total_pretransfer_time / stats->tile_count;
//         stats->avg_starttransfer_time = stats->total_starttransfer_time / stats->tile_count;
//         stats->avg_total_time = stats->total_time / stats->tile_count;
        
//         stats->avg_download_speed = stats->avg_download_speed / stats->tile_count;
//         stats->avg_size = stats->total_size / stats->tile_count;
//         stats->avg_header_size = stats->avg_header_size / stats->tile_count;
//         stats->avg_redirect_time_ms = stats->avg_redirect_time_ms / stats->tile_count;

//         if (stats->tile_count > 1 && timing_array) {
//             double variance = 0.0;
//             double mean = (double)stats->avg_total_time;
//             for (int i = 0; i < stats->tile_count; i++) {
//                 double diff = (double)timing_array[i] - mean;
//                 variance += diff * diff;
//             }
//             variance /= stats->tile_count;
//             stats->jitter_ms = sqrt(variance) / 1000.0;
//         }
//     }
    
//     if (timing_array) free(timing_array);
// }

// RET request_handler_v2_post_get_info(request_handler_v2_t *self,
//                                      COUNT chunk_id,
//                                      int *vp_tiles,
//                                      int num_vp_tiles,
//                                      int *chosen_versions,
//                                      HTTP_VERSION protocol)
// {
//     if (self == NULL || self->pool == NULL) return RET_FAIL;

//     const char *protocol_name = (protocol == STREAM_HTTP_2_0) ? "HTTP/2" : 
//                                 (protocol == STREAM_HTTP_3_0) ? "HTTP/3" : "HTTP/1.1";
//     printf("\n========== SEGMENT %llu (%s) - PARALLEL MODE ==========\n", 
//            chunk_id + 1, protocol_name);

//     int non_vp_tiles[NO_OF_ROWS * NO_OF_COLS];
//     int non_vp_count = 0;
//     for (COUNT tile_id = 0; tile_id < self->tile_count; tile_id++) {
//         bool is_viewport_tile = false;
//         for (int i = 0; i < num_vp_tiles; i++) {
//             if (vp_tiles[i] == tile_id) { 
//                 is_viewport_tile = true; 
//                 break; 
//             }
//         }
//         if (!is_viewport_tile) {
//             non_vp_tiles[non_vp_count++] = tile_id;
//         }
//     }

//     int total_jobs = num_vp_tiles + non_vp_count;
//     download_task_t *jobs = (download_task_t *)calloc(total_jobs, sizeof(download_task_t));
//     if (jobs == NULL) {
//         printf("  ERROR: Failed to allocate download tasks\n");
//         return RET_FAIL;
//     }

//     char **url_buffers = (char **)calloc(total_jobs, sizeof(char *));
//     if (url_buffers == NULL) {
//         free(jobs);
//         return RET_FAIL;
//     }

//     char url_buffer[512];

//     printf("[POOL] Preparing %d VIEWPORT tiles...\n", num_vp_tiles);
//     for (int i = 0; i < num_vp_tiles; i++) {
//         int tile_id = vp_tiles[i];
//         int qp = tile_version_to_num(chosen_versions[i]);
//         snprintf(url_buffer, sizeof(url_buffer),
//                  "%s/Class3_RollerCoaster/Class3_RollerCoaster_%llu/erp_8x6/tile_yuv/tile_%d_%d_480x341.333333333333_QP%d.bin",
//                  self->ser_addr, chunk_id + 1,
//                  tile_id / NO_OF_COLS, tile_id % NO_OF_COLS, qp);

//         url_buffers[i] = strdup(url_buffer);
//         jobs[i].url = url_buffers[i];
//         jobs[i].tile_id = tile_id;
//         jobs[i].status = RET_FAIL;

//         jobs[i].data = &self->data[tile_id];
//         jobs[i].dls = &self->dls[tile_id];
//         jobs[i].cnnt = &self->cnnt[tile_id];
//         jobs[i].pre_trans_time = &self->pre_trans_time[tile_id];
//         jobs[i].start_trans_time = &self->start_trans_time[tile_id];
//         jobs[i].total_time = &self->total_time[tile_id];
//         jobs[i].size_dl = &self->size_dl[tile_id];
//         jobs[i].namelookup_time = &self->namelookup_time[tile_id];
//         jobs[i].appconnect_time = &self->appconnect_time[tile_id];
//         jobs[i].redirect_time = &self->redirect_time[tile_id];
//         jobs[i].header_size = &self->header_size[tile_id];
//         jobs[i].redirect_count = &self->redirect_count[tile_id];
//         jobs[i].connection_reused = &self->connection_reused[tile_id];
//     }

//     printf("[POOL] Preparing %d NON-VIEWPORT tiles...\n", non_vp_count);
//     for (int i = 0; i < non_vp_count; i++) {
//         int tile_id = non_vp_tiles[i];
//         int job_index = num_vp_tiles + i;
//         snprintf(url_buffer, sizeof(url_buffer),
//                  "%s/Class3_RollerCoaster/Class3_RollerCoaster_%llu/erp_8x6/tile_yuv/tile_%d_%d_480x341.333333333333_QP38.bin",
//                  self->ser_addr, chunk_id + 1,
//                  tile_id / NO_OF_COLS, tile_id % NO_OF_COLS);

// //%s/beach/beach_%llu/erp_8x6/tile_yuv/tile_%d_%d_480x360_QP38.bin
                 
//         url_buffers[job_index] = strdup(url_buffer);
//         jobs[job_index].url = url_buffers[job_index];
//         jobs[job_index].tile_id = tile_id;
//         jobs[job_index].status = RET_FAIL;

//         jobs[job_index].data = &self->data[tile_id];
//         jobs[job_index].dls = &self->dls[tile_id];
//         jobs[job_index].cnnt = &self->cnnt[tile_id];
//         jobs[job_index].pre_trans_time = &self->pre_trans_time[tile_id];
//         jobs[job_index].start_trans_time = &self->start_trans_time[tile_id];
//         jobs[job_index].total_time = &self->total_time[tile_id];
//         jobs[job_index].size_dl = &self->size_dl[tile_id];
//         jobs[job_index].namelookup_time = &self->namelookup_time[tile_id];
//         jobs[job_index].appconnect_time = &self->appconnect_time[tile_id];
//         jobs[job_index].redirect_time = &self->redirect_time[tile_id];
//         jobs[job_index].header_size = &self->header_size[tile_id];
//         jobs[job_index].redirect_count = &self->redirect_count[tile_id];
//         jobs[job_index].connection_reused = &self->connection_reused[tile_id];
//     }

//     printf("[POOL] Executing %d total jobs in parallel...\n", total_jobs);
//     if (http_pool_get_parallel(self->pool, jobs, total_jobs, 
//                                self->max_parallel_downloads) != RET_SUCCESS) {
//         printf("  ERROR: http_pool_get_parallel failed\n");
//         for(int i=0; i<total_jobs; i++) free(url_buffers[i]);
//         free(url_buffers);
//         free(jobs);
//         return RET_FAIL;
//     }
//     printf("[POOL] All jobs completed.\n");

//     tile_group_stats_t vp_stats;
//     calculate_group_stats(self, vp_tiles, num_vp_tiles, &vp_stats);
    
//     printf("\n[VIEWPORT SUMMARY]\n");
//     // printf("  Protocol         : %s\n", protocol_name);
//     // printf("  Tiles            : %d\n", vp_stats.tile_count);
//     // printf("  Total Size       : %.2f MB (Headers: %.2f KB)\n", 
//     //        vp_stats.total_size / 1048576.0,
//     //        vp_stats.avg_header_size / 1024.0);
//     // printf("  Avg Tile Size    : %.2f KB\n", vp_stats.avg_size / 1024.0);
    
//     // printf("\n  Speed Statistics:\n");
//     // printf("    Average        : %.2f Mbps\n", vp_stats.avg_download_speed / 1000000.0);
//     // printf("    Min            : %.2f Mbps\n", 
//     //        (vp_stats.min_download_speed == UINT64_MAX) ? 0.0 : 
//     //        (vp_stats.min_download_speed / 1000000.0));
//     // printf("    Max            : %.2f Mbps\n", vp_stats.max_download_speed / 1000000.0);
    
//     // printf("\n  Connection Metrics:\n");
//     // printf("    Connections Reused: %d / %d (%.1f%%)\n", 
//     //        vp_stats.num_connections_reused, vp_stats.tile_count,
//     //        (vp_stats.tile_count > 0) ? 
//     //        (vp_stats.num_connections_reused * 100.0 / vp_stats.tile_count) : 0);
//     // printf("    Redirects      : %d total (avg %.2f ms)\n",
//     //        vp_stats.total_redirects, vp_stats.avg_redirect_time_ms);

//     // printf("\n  CURL Timings (avg, cumulative from start):\n");
//     // printf("    NAMELOOKUP     : %.2f ms\n", vp_stats.avg_namelookup_time / 1000.0);
//     // printf("    CONNECT        : %.2f ms\n", vp_stats.avg_connect_time / 1000.0);
//     // printf("    APPCONNECT     : %.2f ms\n", vp_stats.avg_appconnect_time / 1000.0);
//     // printf("    PRETRANSFER    : %.2f ms\n", vp_stats.avg_pretransfer_time / 1000.0);
//     // printf("    STARTTRANSFER  : %.2f ms\n", vp_stats.avg_starttransfer_time / 1000.0);
//     // printf("    TOTAL          : %.2f ms\n", vp_stats.avg_total_time / 1000.0);
    
//     // printf("\n  Parallel Performance:\n");
//     // printf("    Total Work Time: %.2f ms\n", vp_stats.total_time / 1000.0);
//     // printf("    Jitter (σ)     : %.2f ms\n", vp_stats.jitter_ms);

//     tile_group_stats_t non_vp_stats;
//     calculate_group_stats(self, non_vp_tiles, non_vp_count, &non_vp_stats);
    
//     printf("\n[NON-VIEWPORT SUMMARY]\n");
//     // printf("  Protocol         : %s\n", protocol_name);
//     // printf("  Tiles            : %d\n", non_vp_stats.tile_count);
//     // printf("  Total Size       : %.2f MB (Headers: %.2f KB)\n",
//     //        non_vp_stats.total_size / 1048576.0,
//     //        non_vp_stats.avg_header_size / 1024.0);
//     // printf("  Avg Tile Size    : %.2f KB\n", non_vp_stats.avg_size / 1024.0);
    
//     // printf("\n  Speed Statistics:\n");
//     // printf("    Average        : %.2f Mbps\n", non_vp_stats.avg_download_speed / 1000000.0);
//     // printf("    Min            : %.2f Mbps\n", 
//     //        (non_vp_stats.min_download_speed == UINT64_MAX) ? 0.0 : 
//     //        (non_vp_stats.min_download_speed / 1000000.0));
//     // printf("    Max            : %.2f Mbps\n", non_vp_stats.max_download_speed / 1000.0);
    
//     // printf("\n  Connection Metrics:\n");
//     // printf("    Connections Reused: %d / %d (%.1f%%)\n", 
//     //        non_vp_stats.num_connections_reused, non_vp_stats.tile_count,
//     //        (non_vp_stats.tile_count > 0) ? 
//     //        (non_vp_stats.num_connections_reused * 100.0 / non_vp_stats.tile_count) : 0);
//     // printf("    Redirects      : %d total (avg %.2f ms)\n",
//     //        non_vp_stats.total_redirects, non_vp_stats.avg_redirect_time_ms);
    
//     // printf("\n  CURL Timings (avg, cumulative from start):\n");
//     // printf("    NAMELOOKUP     : %.2f ms\n", non_vp_stats.avg_namelookup_time / 1000.0);
//     // printf("    CONNECT        : %.2f ms\n", non_vp_stats.avg_connect_time / 1000.0);
//     // printf("    APPCONNECT     : %.2f ms\n", non_vp_stats.avg_appconnect_time / 1000.0);
//     // printf("    PRETRANSFER    : %.2f ms\n", non_vp_stats.avg_pretransfer_time / 1000.0);
//     // printf("    STARTTRANSFER  : %.2f ms\n", non_vp_stats.avg_starttransfer_time / 1000.0);
//     // printf("    TOTAL          : %.2f ms\n", non_vp_stats.avg_total_time / 1000.0);
    
//     // printf("\n  Parallel Performance:\n");
//     // printf("    Total Work Time: %.2f ms\n", non_vp_stats.total_time / 1000.0);
//     // printf("    Jitter (σ)     : %.2f ms\n", non_vp_stats.jitter_ms);

//     int64_t total_bytes = vp_stats.total_size + non_vp_stats.total_size;
//     count_time_t total_time = vp_stats.total_time + non_vp_stats.total_time;
//     int total_tiles = vp_stats.tile_count + non_vp_stats.tile_count;
//     int total_reused = vp_stats.num_connections_reused + non_vp_stats.num_connections_reused;
    
//     printf("\n[SEGMENT %llu OVERALL]\n", chunk_id + 1);
//     printf("  Protocol         : %s\n", protocol_name);
//     printf("  Total Tiles      : %d (VP: %d, Non-VP: %d)\n", 
//            total_tiles, vp_stats.tile_count, non_vp_stats.tile_count);
//     printf("  Total Size       : %.2f MB\n", total_bytes / 1048576.0);
//     printf("  Total Work Time  : %.2f ms\n", total_time / 1000.0);
//     printf("  Throughput       : %.2f Mbps\n", 
//            (total_bytes * 8.0) / (total_time / 1000000.0) / 1000000.0);
//     printf("  Conn Reuse Rate  : %.1f%% (%d/%d)\n",
//            (total_tiles > 0) ? (total_reused * 100.0 / total_tiles) : 0,
//            total_reused, total_tiles);
//     printf("===============================\n\n");

//     FILE *csv_file = fopen("http_metrics_v2.csv", "a");
//     if (csv_file) {
//         fseek(csv_file, 0, SEEK_END);
//         if (ftell(csv_file) == 0) {
//             fprintf(csv_file, "segment_id,protocol,tile_type,num_tiles,"
//                    "total_size_mb,avg_size_kb,avg_header_kb,"
//                    "avg_speed_mbps,min_speed_mbps,max_speed_mbps,"
//                    "namelookup_ms,connect_ms,appconnect_ms,pretransfer_ms,starttransfer_ms,total_ms,"
//                    "parallel_total_ms,jitter_ms,"
//                    "conn_reused,conn_reuse_pct,redirects,redirect_time_ms\n");
//         }

//         fprintf(csv_file, "%llu,%s,viewport,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f,%d,%.1f,%d,%.2f\n",
//                 chunk_id + 1, protocol_name, vp_stats.tile_count,
//                 vp_stats.total_size / 1048576.0, vp_stats.avg_size / 1024.0,
//                 vp_stats.avg_header_size / 1024.0,
//                 vp_stats.avg_download_speed / 1000000.0,
//                 (vp_stats.min_download_speed == UINT64_MAX) ? 0.0 : 
//                 (vp_stats.min_download_speed / 1000000.0),
//                 vp_stats.max_download_speed / 1000000.0,
//                 vp_stats.avg_namelookup_time / 1000.0,
//                 vp_stats.avg_connect_time / 1000.0,
//                 vp_stats.avg_appconnect_time / 1000.0,
//                 vp_stats.avg_pretransfer_time / 1000.0,
//                 vp_stats.avg_starttransfer_time / 1000.0,
//                 vp_stats.avg_total_time / 1000.0,
//                 vp_stats.total_time / 1000.0, vp_stats.jitter_ms,
//                 vp_stats.num_connections_reused,
//                 (vp_stats.tile_count > 0) ? 
//                 (vp_stats.num_connections_reused * 100.0 / vp_stats.tile_count) : 0,
//                 vp_stats.total_redirects, vp_stats.avg_redirect_time_ms);

//         fprintf(csv_file, "%llu,%s,non-viewport,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f,%d,%.1f,%d,%.2f\n",
//                 chunk_id + 1, protocol_name, non_vp_stats.tile_count,
//                 non_vp_stats.total_size / 1048576.0, non_vp_stats.avg_size / 1024.0,
//                 non_vp_stats.avg_header_size / 1024.0,
//                 non_vp_stats.avg_download_speed / 1000000.0,
//                 (non_vp_stats.min_download_speed == UINT64_MAX) ? 0.0 : 
//                 (non_vp_stats.min_download_speed / 1000000.0),
//                 non_vp_stats.max_download_speed / 1000000.0,
//                 non_vp_stats.avg_namelookup_time / 1000.0,
//                 non_vp_stats.avg_connect_time / 1000.0,
//                 non_vp_stats.avg_appconnect_time / 1000.0,
//                 non_vp_stats.avg_pretransfer_time / 1000.0,
//                 non_vp_stats.avg_starttransfer_time / 1000.0,
//                 non_vp_stats.avg_total_time / 1000.0,
//                 non_vp_stats.total_time / 1000.0, non_vp_stats.jitter_ms,
//                 non_vp_stats.num_connections_reused,
//                 (non_vp_stats.tile_count > 0) ? 
//                 (non_vp_stats.num_connections_reused * 100.0 / non_vp_stats.tile_count) : 0,
//                 non_vp_stats.total_redirects, non_vp_stats.avg_redirect_time_ms);
        
//         fclose(csv_file);
//     }

//     for(int i=0; i < total_jobs; i++) {
//         if(url_buffers[i]) free(url_buffers[i]);
//     }
//     free(url_buffers);
//     free(jobs);

//     return RET_SUCCESS;
// }

// RET request_handler_v2_reset(request_handler_v2_t *self) {
//     if (!self) return RET_FAIL;
    
//     for (COUNT i = 0; i < self->tile_count; i++) {
//         buffer_destroy(&self->data[i]);
//         buffer_init(&self->data[i]);

//         self->dls[i] = 0;
//         self->cnnt[i] = 0;
//         self->pre_trans_time[i] = 0;
//         self->start_trans_time[i] = 0;
//         self->total_time[i] = 0;
//         self->size_dl[i] = 0;
//         self->namelookup_time[i] = 0;
//         self->appconnect_time[i] = 0;
//         self->redirect_time[i] = 0;
//         self->header_size[i] = 0;
//         self->redirect_count[i] = 0;
//         self->connection_reused[i] = 0;
//     }

//     return RET_SUCCESS;
// }

// RET request_handler_v2_destroy(request_handler_v2_t *self) {
//     if (!self) return RET_FAIL;
    
//     if(self->pool) {
//         http_pool_destroy(self->pool);
//         free(self->pool);
//     }
    
//     if (self->data) {
//         for (COUNT i = 0; i < self->tile_count; i++) {
//             buffer_destroy(&self->data[i]);
//         }
//         free(self->data);
//     }
    
//     free(self->dls);
//     free(self->cnnt);
//     free(self->pre_trans_time);
//     free(self->start_trans_time);
//     free(self->total_time);
//     free(self->size_dl);
//     free(self->namelookup_time);
//     free(self->appconnect_time);
//     free(self->redirect_time);
//     free(self->header_size);
//     free(self->redirect_count);
//     free(self->connection_reused);
    
//     memset(self, 0, sizeof(request_handler_v2_t));
//     return RET_SUCCESS;
// }

// int tile_version_to_num(int version)
// {
//   switch (version)
//   {
//   case 0:
//     return 38;
//     break;
//   case 1:
//     return 32;
//     break;
//   case 2:
//     return 28;
//     break;
//   case 3:
//     return 24;
//     break;
//   case 4:
//     return 20;
//     break;
//   default:
//     break;
//   }
// }

/**
 * request_handler_v2.c  (updated — Tasks 3 & 4)
 *
 * Changes from the original:
 *
 *   1. request_handler_v2_init()
 *      Allocates p_map[] alongside the existing metric arrays.
 *
 *   2. request_handler_v2_post_get_info()
 *      • If self->sched_output is non-NULL, chosen quality levels come from
 *        sched_output->tiles[tile_id].chosen_version (stochastic scheduler).
 *      • Each download_task_t.prob_ptr is pointed at self->p_map[tile_id]
 *        so http_pool's polling loop can perform early termination in real
 *        time when viewport probability drops below τ.
 *      • After the parallel download, counts tiles marked RET_EARLY_TERMINATED
 *        and accumulates them in self->early_term_count.
 *
 *   3. request_handler_v2_update_pmap()
 *      Copies a fresh probability array into self->p_map.
 *
 *   4. request_handler_v2_reset() / destroy()
 *      Reset / free p_map together with the other arrays.
 */

#include "proto_comp/request_handler_v2.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── init ───────────────────────────────────────────────────────────────── */
RET request_handler_v2_init(request_handler_v2_t *self,
                            const char           *ser_adrr,
                            COUNT                 seg_count,
                            COUNT                 version_count,
                            COUNT                 tile_count,
                            HTTP_VERSION          protocol,
                            int                   max_parallel_downloads)
{
    if (!self || !ser_adrr) return RET_FAIL;

    memset(self, 0, sizeof(request_handler_v2_t));
    self->ser_addr               = (char *)ser_adrr;
    self->seg_count              = seg_count;
    self->tile_count             = tile_count;
    self->version_count          = version_count;
    self->max_parallel_downloads = max_parallel_downloads;
    self->cts_out                = NULL;
    self->early_term_count       = 0;

    self->pool = (http_pool_t *)malloc(sizeof(http_pool_t));
    if (!self->pool) return RET_FAIL;

    if (http_pool_init(self->pool, protocol, max_parallel_downloads)
        != RET_SUCCESS) {
        free(self->pool);
        return RET_FAIL;
    }

#define ALLOC(field, type) \
    self->field = (type *)calloc(tile_count, sizeof(type)); \
    if (!self->field) goto cleanup;

    self->data = (buffer_t *)calloc(tile_count, sizeof(buffer_t));
    if (!self->data) goto cleanup;
    for (COUNT i = 0; i < tile_count; i++) {
        if (buffer_init(&self->data[i]) != RET_SUCCESS) {
            for (COUNT j = 0; j < i; j++) buffer_destroy(&self->data[j]);
            goto cleanup;
        }
    }

    ALLOC(dls,               bw_t)
    ALLOC(cnnt,              count_time_t)
    ALLOC(pre_trans_time,    count_time_t)
    ALLOC(start_trans_time,  count_time_t)
    ALLOC(total_time,        count_time_t)
    ALLOC(size_dl,           count_time_t)
    ALLOC(namelookup_time,   count_time_t)
    ALLOC(appconnect_time,   count_time_t)
    ALLOC(redirect_time,     count_time_t)
    ALLOC(header_size,       size_t)
    ALLOC(redirect_count,    count_t)
    ALLOC(connection_reused, int)

    /* probability map — initially 0; filled by update_pmap() each segment */
    self->p_map = (float *)calloc(tile_count, sizeof(float));
    if (!self->p_map) goto cleanup;
#undef ALLOC

    return RET_SUCCESS;

cleanup:
    request_handler_v2_destroy(self);
    return RET_FAIL;
}

/* ── update_pmap ────────────────────────────────────────────────────────── */
RET request_handler_v2_update_pmap(request_handler_v2_t *self,
                                   const float          *p_map_src)
{
    if (!self || !self->p_map || !p_map_src) return RET_FAIL;
    memcpy(self->p_map, p_map_src, self->tile_count * sizeof(float));
    return RET_SUCCESS;
}

/* ── group statistics (unchanged logic) ─────────────────────────────────── */
static void calc_group_stats(request_handler_v2_t *self,
                             int *tile_ids, int num,
                             tile_group_stats_t *s)
{
    memset(s, 0, sizeof(*s));
    s->min_download_speed = UINT64_MAX;

    count_time_t *ta = (num > 1)
        ? (count_time_t *)malloc(num * sizeof(count_time_t)) : NULL;

    for (int i = 0; i < num; i++) {
        int tid = tile_ids[i];
        if (self->size_dl[tid] == 0) continue;

        s->total_namelookup_time    += self->namelookup_time[tid];
        s->total_connect_time       += self->cnnt[tid];
        s->total_appconnect_time    += self->appconnect_time[tid];
        s->total_pretransfer_time   += self->pre_trans_time[tid];
        s->total_starttransfer_time += self->start_trans_time[tid];
        s->total_time               += self->total_time[tid];
        s->avg_download_speed       += self->dls[tid];
        if (self->dls[tid] < s->min_download_speed) s->min_download_speed = self->dls[tid];
        if (self->dls[tid] > s->max_download_speed) s->max_download_speed = self->dls[tid];
        s->total_size               += self->size_dl[tid];
        s->avg_header_size          += self->header_size[tid];
        s->num_connections_reused   += self->connection_reused[tid];
        s->total_redirects          += self->redirect_count[tid];
        s->avg_redirect_time_ms     += self->redirect_time[tid] / 1000.0;
        if (ta) ta[s->tile_count] = self->total_time[tid];
        s->tile_count++;
    }

    if (s->tile_count > 0) {
        s->avg_namelookup_time    = s->total_namelookup_time    / s->tile_count;
        s->avg_connect_time       = s->total_connect_time       / s->tile_count;
        s->avg_appconnect_time    = s->total_appconnect_time    / s->tile_count;
        s->avg_pretransfer_time   = s->total_pretransfer_time   / s->tile_count;
        s->avg_starttransfer_time = s->total_starttransfer_time / s->tile_count;
        s->avg_total_time         = s->total_time               / s->tile_count;
        s->avg_download_speed     = s->avg_download_speed       / s->tile_count;
        s->avg_size               = s->total_size               / s->tile_count;
        s->avg_header_size        = s->avg_header_size          / s->tile_count;
        s->avg_redirect_time_ms   = s->avg_redirect_time_ms     / s->tile_count;

        if (s->tile_count > 1 && ta) {
            double var = 0.0, mean = (double)s->avg_total_time;
            for (int i = 0; i < s->tile_count; i++) {
                double d = (double)ta[i] - mean;
                var += d * d;
            }
            s->jitter_ms = sqrt(var / s->tile_count) / 1000.0;
        }
    }
    if (ta) free(ta);
}

static bool is_tile_in_actual_viewport(int tile_id, float actual_yaw, float actual_pitch) {
    int row = tile_id / NO_OF_COLS;
    int col = tile_id % NO_OF_COLS;
    float ty = (col + 0.5f) * TILE_WIDTH - 180.0f;
    float tp = (row + 0.5f) * TILE_HEIGHT - 90.0f;
    float dy = fabsf(actual_yaw - ty);
    if (dy > 180.0f) dy = 360.0f - dy;
    float dp = fabsf(actual_pitch - tp);
    return (dy <= 45.0f && dp <= 45.0f); // Giả sử Viewport 100x100 độ
}

/* ── post_get_info ──────────────────────────────────────────────────────── */
RET request_handler_v2_post_get_info(request_handler_v2_t *self,
                                     COUNT                  chunk_id,
                                     int                   *vp_tiles,
                                     int                    num_vp_tiles,
                                     int                   *chosen_versions,
                                     float                  actual_yaw,   
                                     float                  actual_pitch,
                                     HTTP_VERSION           protocol)
{
    if (!self || !self->pool) return RET_FAIL;

    const char *pname = (protocol == STREAM_HTTP_2_0) ? "HTTP/2" :
                        (protocol == STREAM_HTTP_3_0) ? "HTTP/3" : "HTTP/1.1";

    printf("\n========== SEGMENT %llu (%s) ==========\n",
           chunk_id + 1, pname);

    /* build non-VP list */
    int non_vp[NO_OF_ROWS * NO_OF_COLS], nvc = 0;
    for (COUNT tid = 0; tid < self->tile_count; tid++) {
        bool isvp = false;
        for (int i = 0; i < num_vp_tiles; i++)
            if (vp_tiles[i] == (int)tid) { isvp = true; break; }
        if (!isvp) non_vp[nvc++] = (int)tid;
    }

    int total_jobs = num_vp_tiles + nvc;
    download_task_t *jobs  = (download_task_t *)calloc(total_jobs, sizeof(*jobs));
    char           **urls  = (char **)calloc(total_jobs, sizeof(char *));
    if (!jobs || !urls) { free(jobs); free(urls); return RET_FAIL; }

    char ubuf[512];

    /* viewport tiles */
    for (int i = 0; i < num_vp_tiles; i++) {
        int  tid = vp_tiles[i];
        int  ver = (self->cts_out)
                       ? self->cts_out->tiles[tid].chosen_version
                       : (chosen_versions ? chosen_versions[i] : 0);
        int  qp  = tile_version_to_num(ver);

        snprintf(ubuf, sizeof(ubuf),
                 "%s/Class3_RollerCoaster/Class3_RollerCoaster_%llu"
                 "/erp_8x6/tile_yuv/tile_%d_%d_480x341.333333333333_QP%d.bin",
                 self->ser_addr, chunk_id + 1,
                 tid / NO_OF_COLS, tid % NO_OF_COLS, qp);

        urls[i]       = strdup(ubuf);
        jobs[i].url   = urls[i];
        jobs[i].tile_id = tid;
        jobs[i].status  = RET_FAIL;
        /* wire probability pointer for early termination */
        jobs[i].prob_ptr = (self->p_map) ? &self->p_map[tid] : NULL;

        jobs[i].data              = &self->data[tid];
        jobs[i].dls               = &self->dls[tid];
        jobs[i].cnnt              = &self->cnnt[tid];
        jobs[i].pre_trans_time    = &self->pre_trans_time[tid];
        jobs[i].start_trans_time  = &self->start_trans_time[tid];
        jobs[i].total_time        = &self->total_time[tid];
        jobs[i].size_dl           = &self->size_dl[tid];
        jobs[i].namelookup_time   = &self->namelookup_time[tid];
        jobs[i].appconnect_time   = &self->appconnect_time[tid];
        jobs[i].redirect_time     = &self->redirect_time[tid];
        jobs[i].header_size       = &self->header_size[tid];
        jobs[i].redirect_count    = &self->redirect_count[tid];
        jobs[i].connection_reused = &self->connection_reused[tid];
    }

    /* non-viewport tiles */
    for (int i = 0; i < nvc; i++) {
        int tid = non_vp[i];
        int ji  = num_vp_tiles + i;
        int ver = (self->cts_out)
                      ? self->cts_out->tiles[tid].chosen_version : 0;
        int qp  = tile_version_to_num(ver);

        snprintf(ubuf, sizeof(ubuf),
                 "%s/Class3_RollerCoaster/Class3_RollerCoaster_%llu"
                 "/erp_8x6/tile_yuv/tile_%d_%d_480x341.333333333333_QP%d.bin",
                 self->ser_addr, chunk_id + 1,
                 tid / NO_OF_COLS, tid % NO_OF_COLS, qp);

        urls[ji]        = strdup(ubuf);
        jobs[ji].url    = urls[ji];
        jobs[ji].tile_id = tid;
        jobs[ji].status  = RET_FAIL;
        jobs[ji].prob_ptr = (self->p_map) ? &self->p_map[tid] : NULL;

        jobs[ji].data              = &self->data[tid];
        jobs[ji].dls               = &self->dls[tid];
        jobs[ji].cnnt              = &self->cnnt[tid];
        jobs[ji].pre_trans_time    = &self->pre_trans_time[tid];
        jobs[ji].start_trans_time  = &self->start_trans_time[tid];
        jobs[ji].total_time        = &self->total_time[tid];
        jobs[ji].size_dl           = &self->size_dl[tid];
        jobs[ji].namelookup_time   = &self->namelookup_time[tid];
        jobs[ji].appconnect_time   = &self->appconnect_time[tid];
        jobs[ji].redirect_time     = &self->redirect_time[tid];
        jobs[ji].header_size       = &self->header_size[tid];
        jobs[ji].redirect_count    = &self->redirect_count[tid];
        jobs[ji].connection_reused = &self->connection_reused[tid];
    }

    printf("[POOL] %d VP + %d non-VP tiles (tau=%.3f)\n",
           num_vp_tiles, nvc, self->pool->early_term_tau);

    if (http_pool_get_parallel(self->pool, jobs, total_jobs,
                               self->max_parallel_downloads) != RET_SUCCESS) {
        fprintf(stderr, "[rh_v2] download failed\n");
        for (int i = 0; i < total_jobs; i++) free(urls[i]);
        free(urls); free(jobs);
        return RET_FAIL;
    }

    /* count early-terminated tiles */
    int et = 0;
    for (int i = 0; i < total_jobs; i++) {
        if (jobs[i].status == RET_EARLY_TERMINATED) {
            et++;
            if (self->cts_out) {
                /* mark in scheduler output (cast away const — it's our own struct) */
                ((cts_tile_t *)&self->cts_out->tiles[jobs[i].tile_id])
                    ->early_terminated = 1;
            }
        }
    }
    self->early_term_count += et;
    if (et) printf("[ET] %d tile(s) early-terminated this segment "
                   "(total=%d)\n", et, self->early_term_count);

    /* statistics */
    tile_group_stats_t vp_s, nv_s;
    calc_group_stats(self, vp_tiles, num_vp_tiles, &vp_s);
    calc_group_stats(self, non_vp,   nvc,          &nv_s);

    int64_t total_bytes = (int64_t)vp_s.total_size + (int64_t)nv_s.total_size;
    count_time_t total_us = vp_s.total_time + nv_s.total_time;
    int all_tiles  = vp_s.tile_count + nv_s.tile_count;
    int all_reused = vp_s.num_connections_reused + nv_s.num_connections_reused;

    printf("[SEG %llu] Proto=%s Tiles=%d(VP:%d NV:%d ET:%d) "
           "Size=%.2fMB Time=%.2fms Tput=%.2fMbps Reuse=%.1f%%\n",
           chunk_id + 1, pname,
           all_tiles, vp_s.tile_count, nv_s.tile_count, et,
           total_bytes / 1048576.0,
           total_us / 1000.0,
           (total_us > 0) ? (total_bytes * 8.0 / (total_us / 1e6) / 1e6) : 0.0,
           (all_tiles > 0) ? (all_reused * 100.0 / all_tiles) : 0.0);
    printf("====================================\n\n");

    /* ── MỚI: Tính toán Wasted Ratio và QoE Drop Rate ── */
    long long total_bytes_downloaded = 0;
    long long wasted_bytes = 0;
    int actual_vp_tiles_count = 0;
    int qoe_dropped_tiles = 0;

    for (int i = 0; i < self->tile_count; i++) {
        bool in_actual_vp = is_tile_in_actual_viewport(i, actual_yaw, actual_pitch);
        if (in_actual_vp) actual_vp_tiles_count++;

        // 1. Wasted Ratio: Đã tải xong nhưng không nằm trong thực tế
        if (self->size_dl[i] > 0) {
            total_bytes_downloaded += self->size_dl[i];
            if (!in_actual_vp) {
                wasted_bytes += self->size_dl[i];
            }
        }

        // 2. QoE Drop: Tile thực tế bị hủy (Early Terminated)
        if (in_actual_vp) {
            // Kiểm tra trạng thái Early Terminated đã được đánh dấu trước đó
            if (self->cts_out && self->cts_out->tiles[i].early_terminated) {
                qoe_dropped_tiles++;
            }
        }
    }

    float wasted_ratio = (total_bytes_downloaded > 0) ? 
                         ((float)wasted_bytes / total_bytes_downloaded) : 0;
    float qoe_drop_rate = (actual_vp_tiles_count > 0) ? 
                          ((float)qoe_dropped_tiles / actual_vp_tiles_count) : 0;

    printf("[METRICS] Wasted Ratio: %.2f%% | QoE Drop Rate: %.2f%%\n", 
           wasted_ratio * 100.0f, qoe_drop_rate * 100.0f);

    /* CSV */
    FILE *csv = fopen("http_metrics_v2.csv", "a");
    if (csv) {
        if (ftell(csv) == 0)
            fprintf(csv, "seg,proto,type,tiles,size_mb,speed_mbps,"
                         "total_ms,jitter_ms,reuse_pct,et\n");
        fprintf(csv, "%llu,%s,viewport,%d,%.4f,%.4f,%.4f,%.2f,%.1f,%d\n",
                chunk_id+1, pname, vp_s.tile_count,
                vp_s.total_size/1048576.0,
                vp_s.avg_download_speed/1e6,
                vp_s.total_time/1000.0, vp_s.jitter_ms,
                (vp_s.tile_count>0) ? (vp_s.num_connections_reused*100.0/vp_s.tile_count) : 0.0,
                et);
        fprintf(csv, "%llu,%s,non-vp,%d,%.4f,%.4f,%.4f,%.2f,%.1f,0\n",
                chunk_id+1, pname, nv_s.tile_count,
                nv_s.total_size/1048576.0,
                nv_s.avg_download_speed/1e6,
                nv_s.total_time/1000.0, nv_s.jitter_ms,
                (nv_s.tile_count>0) ? (nv_s.num_connections_reused*100.0/nv_s.tile_count) : 0.0);
        fprintf(csv, "%llu,%s,summary,%.4f,%.4f\n", 
                chunk_id + 1, pname, wasted_ratio, qoe_drop_rate);
        fclose(csv);
    }

    for (int i = 0; i < total_jobs; i++) free(urls[i]);
    free(urls);
    free(jobs);
    return RET_SUCCESS;
}

/* ── reset ──────────────────────────────────────────────────────────────── */
RET request_handler_v2_reset(request_handler_v2_t *self)
{
    if (!self) return RET_FAIL;
    for (COUNT i = 0; i < self->tile_count; i++) {
        buffer_destroy(&self->data[i]);
        buffer_init(&self->data[i]);
        self->dls[i] = self->cnnt[i] = self->pre_trans_time[i] = 0;
        self->start_trans_time[i] = self->total_time[i] = self->size_dl[i] = 0;
        self->namelookup_time[i]  = self->appconnect_time[i] = 0;
        self->redirect_time[i]    = 0;
        self->header_size[i]      = 0;
        self->redirect_count[i]   = 0;
        self->connection_reused[i]= 0;
        /* p_map is preserved — holds latest estimate */
    }
    self->cts_out = NULL;
    return RET_SUCCESS;
}

/* ── destroy ────────────────────────────────────────────────────────────── */
RET request_handler_v2_destroy(request_handler_v2_t *self)
{
    if (!self) return RET_FAIL;
    if (self->pool) { http_pool_destroy(self->pool); free(self->pool); }
    if (self->data) {
        for (COUNT i = 0; i < self->tile_count; i++)
            buffer_destroy(&self->data[i]);
        free(self->data);
    }
    free(self->dls);         free(self->cnnt);
    free(self->pre_trans_time); free(self->start_trans_time);
    free(self->total_time);  free(self->size_dl);
    free(self->namelookup_time); free(self->appconnect_time);
    free(self->redirect_time);   free(self->header_size);
    free(self->redirect_count);  free(self->connection_reused);
    free(self->p_map);
    memset(self, 0, sizeof(*self));
    return RET_SUCCESS;
}

int tile_version_to_num(int version)
{
    switch (version) {
    case 0: return 38; case 1: return 32; case 2: return 28;
    case 3: return 24; case 4: return 20; default: return 38;
    }
}