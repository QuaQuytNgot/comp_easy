#ifndef HTTP_POOL_H
#define HTTP_POOL_H

#include "define.h"
#include "buffer.h"
#include <curl/curl.h>
#include "http.h"

/* Connection pool structure to reuse CURL handles */
typedef struct http_pool_t {
    CURL *curl_handle;           // Reusable CURL handle
    CURLSH *share_handle;        // Share DNS cache, connections, SSL sessions
    HTTP_VERSION version;
    int is_initialized;
} http_pool_t;

/* Structure for parallel downloads */
typedef struct download_task_t {
    const char *url;
    buffer_t *data;
    bw_t *dls;
    count_time_t *cnnt;
    count_time_t *pre_trans_time;
    count_time_t *start_trans_time;
    count_time_t *total_time;
    count_time_t *size_dl;
    CURL *easy_handle;
    int tile_id;
    RET status;
} download_task_t;

/* Initialize connection pool */
RET http_pool_init(http_pool_t *pool, HTTP_VERSION ver);

/* Destroy connection pool */
RET http_pool_destroy(http_pool_t *pool);

/* Single download using pooled connection */
RET http_pool_get(http_pool_t *pool,
                  const char *url,
                  buffer_t *data,
                  bw_t *dls,
                  count_time_t *cnnt,
                  count_time_t *pre_trans_time,
                  count_time_t *start_trans_time,
                  count_time_t *total_time,
                  count_time_t *size_dl);

/* Parallel download multiple URLs */
RET http_pool_get_parallel(http_pool_t *pool,
                           download_task_t *tasks,
                           int num_tasks,
                           int max_concurrent);

#endif