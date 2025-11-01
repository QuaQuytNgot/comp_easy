#ifndef REQUEST_HANDLER_V2_H
#define REQUEST_HANDLER_V2_H

#include "buffer.h"
#include "define.h"
#include "http_pool.h"

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

RET request_handler_v2_init(request_handler_v2_t *self,
                            const char *ser_addr,
                            COUNT seg_count,
                            COUNT version_count,
                            COUNT tile_count,
                            HTTP_VERSION http_ver,
                            int max_parallel);

RET request_handler_v2_post_get_info(request_handler_v2_t *self,
                                     COUNT chunk_id,
                                     int *vp_tiles,
                                     int num_vp_tiles,
                                     int *chosen_versions);

RET request_handler_v2_reset(request_handler_v2_t *self);
RET request_handler_v2_destroy(request_handler_v2_t *self);

#endif