// #ifndef HTTP_POOL_H
// #define HTTP_POOL_H

// #include "define.h"
// #include "buffer.h"
// #include <curl/curl.h>
// #include "http.h"

// /* Connection pool structure to reuse CURL handles */
// typedef struct http_pool_t {
//     CURLSH *share_handle;        // Share DNS cache, connections, SSL sessions
//     HTTP_VERSION version;
//     int max_parallel_downloads;
//     int is_initialized;
// } http_pool_t;

// /* Structure for parallel downloads */
// typedef struct download_task_t {
//     const char *url;
//     buffer_t *data;
//     bw_t *dls;
//     count_time_t *cnnt;
//     count_time_t *pre_trans_time;
//     count_time_t *start_trans_time;
//     count_time_t *total_time;
//     count_time_t *size_dl;
//     count_time_t *namelookup_time;
//     count_time_t *appconnect_time;
//     int *connection_reused;
//     // Additional metrics
//     count_time_t *redirect_time;
//     size_t *header_size;
//     int *redirect_count;
//     CURL *easy_handle;
//     int tile_id;
//     RET status;
// } download_task_t;

// /* Initialize connection pool */
// RET http_pool_init(http_pool_t *pool, HTTP_VERSION ver, int max_parallel_downloads);

// /* Destroy connection pool */
// RET http_pool_destroy(http_pool_t *pool);

// /* Parallel download multiple URLs */
// RET http_pool_get_parallel(http_pool_t *pool,
//                            download_task_t *tasks,
//                            int num_tasks,
//                            int max_concurrent);

// #endif

#ifndef HTTP_POOL_H
#define HTTP_POOL_H

#include "define.h"
#include "buffer.h"
#include <curl/curl.h>
#include "http.h"
#include "resource_monitor.h"

/* ─────────────────────────────────────────────────────────────────────────
 * Early-termination sentinel written to download_task_t.status when a tile
 * is cancelled because p_i(t) dropped below the pool's tau threshold.
 * Value 2 never collides with RET_FAIL (0) or RET_SUCCESS (1).
 * ───────────────────────────────────────────────────────────────────────── */
#define RET_EARLY_TERMINATED  2u

/* Default probability threshold τ.  Set pool->early_term_tau = 0.0f to
 * disable early termination entirely. */
#define HTTP_POOL_TAU_DEFAULT 0.10f

/* ─────────────────────────────────────────────────────────────────────────
 * Connection pool — wraps CURLSH for DNS / SSL / connection sharing.
 *
 * Added vs original:  early_term_tau
 * ───────────────────────────────────────────────────────────────────────── */
typedef struct http_pool_t {
    CURLSH       *share_handle;
    HTTP_VERSION  version;
    int           max_parallel_downloads;
    int           is_initialized;

    /* Stochastic early-termination threshold τ (0.0f = disabled) */
    float         early_term_tau;
} http_pool_t;

/* ─────────────────────────────────────────────────────────────────────────
 * Per-tile download task.
 *
 * Added vs original:  prob_ptr
 *   Points into the live probability map p_i(t).  The pool's polling loop
 *   reads *prob_ptr on every curl_multi_poll wakeup; if it falls below
 *   early_term_tau the transfer is cancelled via curl_multi_remove_handle()
 *   (sends RESET_STREAM for HTTP/3, RST_STREAM for HTTP/2).
 *   Pass NULL to skip early termination for this task.
 * ───────────────────────────────────────────────────────────────────────── */
typedef struct download_task_t {
    const char   *url;
    buffer_t     *data;
    bw_t         *dls;
    count_time_t *cnnt;
    count_time_t *pre_trans_time;
    count_time_t *start_trans_time;
    count_time_t *total_time;
    count_time_t *size_dl;
    count_time_t *namelookup_time;
    count_time_t *appconnect_time;
    int          *connection_reused;
    /* Additional metrics */
    count_time_t *redirect_time;
    size_t       *header_size;
    int          *redirect_count;
    CURL         *easy_handle;
    int           tile_id;
    RET           status;

    /* Live p_i(t) pointer for early termination — NULL disables the check */
    const float  *prob_ptr;

    int           is_dispatched;
} download_task_t;

/* Initialize connection pool */
RET http_pool_init(http_pool_t *pool, HTTP_VERSION ver,
                   int max_parallel_downloads);

/* Destroy connection pool */
RET http_pool_destroy(http_pool_t *pool);

/* Parallel download with optional per-task early termination */
RET http_pool_get_parallel(http_pool_t     *pool,
                           download_task_t *tasks,
                           int              num_tasks,
                           int              max_concurrent);

RET http_pool_get_parallel_dynamic(http_pool_t     *pool,
                                download_task_t *tasks,
                                int              num_tasks,
                                int              max_concurrent,
                                resource_monitor_t *rm);

#endif /* HTTP_POOL_H */