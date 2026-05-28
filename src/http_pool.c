// #include "proto_comp/http_pool.h"
// #include <string.h>
// #include <stdio.h>

// /* Lock callback for thread-safe sharing */
// static void lock_cb(CURL *handle, curl_lock_data data,
//                    curl_lock_access access, void *userptr) {
//     (void)handle;
//     (void)data;
//     (void)access;
//     (void)userptr;
// }

// static void unlock_cb(CURL *handle, curl_lock_data data, void *userptr) {
//     (void)handle;
//     (void)data;
//     (void)userptr;
// }

// RET http_pool_init(http_pool_t *pool, HTTP_VERSION ver, int max_parallel_downloads) {
//     if (!pool) return RET_FAIL;

//     memset(pool, 0, sizeof(http_pool_t));

//     /* Create share handle for connection reuse */
//     pool->share_handle = curl_share_init();
//     if (!pool->share_handle) {
//         return RET_FAIL;
//     }

//     /* Share DNS cache, SSL sessions, and connections */
//     curl_share_setopt(pool->share_handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
//     curl_share_setopt(pool->share_handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
//     curl_share_setopt(pool->share_handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);

//     /* Set lock functions (for thread safety) */
//     curl_share_setopt(pool->share_handle, CURLSHOPT_LOCKFUNC, lock_cb);
//     curl_share_setopt(pool->share_handle, CURLSHOPT_UNLOCKFUNC, unlock_cb);

//     pool->version = ver;
//     pool->max_parallel_downloads = max_parallel_downloads;
//     pool->is_initialized = 1;

//     return RET_SUCCESS;
// }

// RET http_pool_destroy(http_pool_t *pool) {
//     if (!pool || !pool->is_initialized) return RET_FAIL;

//     if (pool->share_handle) {
//         curl_share_cleanup(pool->share_handle);
//     }

//     memset(pool, 0, sizeof(http_pool_t));
//     return RET_SUCCESS;
// }

// /* Parallel download using CURL Multi Interface */
// RET http_pool_get_parallel(http_pool_t *pool,
//                            download_task_t *tasks,
//                            int num_tasks,
//                            int max_concurrent) {

//     if (!pool || !pool->is_initialized || !tasks || num_tasks <= 0) {
//         return RET_FAIL;
//     }

//     CURLM *multi_handle = curl_multi_init();
//     if (!multi_handle) {
//         return RET_FAIL;
//     }

//     /* Set max concurrent connections */
//     curl_multi_setopt(multi_handle, CURLMOPT_MAXCONNECTS, max_concurrent);
//     curl_multi_setopt(multi_handle, CURLMOPT_MAX_HOST_CONNECTIONS, max_concurrent);

//     /* Enable HTTP/2 multiplexing (if available) */
//     curl_multi_setopt(multi_handle, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);

//     /* Create easy handles for each task */
//     for (int i = 0; i < num_tasks; i++) {
//         tasks[i].easy_handle = curl_easy_init();
//         if (!tasks[i].easy_handle) {
//             /* Cleanup on failure */
//             for (int j = 0; j < i; j++) {
//                 curl_easy_cleanup(tasks[j].easy_handle);
//             }
//             curl_multi_cleanup(multi_handle);
//             return RET_FAIL;
//         }

//         CURL *eh = tasks[i].easy_handle;

//         /* Reset buffer */
//         buffer_destroy(tasks[i].data);
//         buffer_init(tasks[i].data);

//         /* Copy common settings from pool */
//         curl_easy_setopt(eh, CURLOPT_SHARE, pool->share_handle);
//         curl_easy_setopt(eh, CURLOPT_BUFFERSIZE, 102400L);
//         curl_easy_setopt(eh, CURLOPT_NOPROGRESS, 1L);
//         curl_easy_setopt(eh, CURLOPT_USERAGENT, "curl/7.91.0");
//         curl_easy_setopt(eh, CURLOPT_MAXREDIRS, 50L);
//         curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, write_callback);
//         curl_easy_setopt(eh, CURLOPT_SSL_VERIFYPEER, 0L);
//         curl_easy_setopt(eh, CURLOPT_SSL_VERIFYHOST, 0L);
//         curl_easy_setopt(eh, CURLOPT_TCP_KEEPALIVE, 1L);
//         curl_easy_setopt(eh, CURLOPT_FTP_SKIP_PASV_IP, 1L);

//         /* Set HTTP version */
//         switch (pool->version) {
//             case STREAM_HTTP_1_1:
//                 curl_easy_setopt(eh, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
//                 break;
//             case STREAM_HTTP_2_0:
//                 curl_easy_setopt(eh, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
//                 break;
//             case STREAM_HTTP_3_0:
//                 curl_easy_setopt(eh, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_3ONLY);
//                 break;
//         }

//         /* Set specific URL and data for this task */
//         curl_easy_setopt(eh, CURLOPT_URL, tasks[i].url);
//         curl_easy_setopt(eh, CURLOPT_WRITEDATA, (void *)tasks[i].data);
//         curl_easy_setopt(eh, CURLOPT_PRIVATE, (void *)(intptr_t)i); // Store task index

//         /* Optional: Enable verbose for first tile to debug */
//         #ifdef DEBUG_FIRST_TILE
//         if (i == 0) {
//             curl_easy_setopt(eh, CURLOPT_VERBOSE, 1L);
//         }
//         #endif

//         /* Add to multi handle */
//         curl_multi_add_handle(multi_handle, eh);
//     }

//     /* Perform parallel transfers */
//     int still_running = 0;
//     do {
//         CURLMcode mc = curl_multi_perform(multi_handle, &still_running);

//         if (mc != CURLM_OK) {
//             fprintf(stderr, "curl_multi_perform error: %s\n",
//                    curl_multi_strerror(mc));
//             break;
//         }

//         /* Wait for activity or timeout */
//         if (still_running) {
//             mc = curl_multi_poll(multi_handle, NULL, 0, 1000, NULL);
//             if (mc != CURLM_OK) {
//                 fprintf(stderr, "curl_multi_poll error: %s\n",
//                        curl_multi_strerror(mc));
//                 break;
//             }
//         }
//     } while (still_running);

//     /* Check results for each transfer */
//     int msgs_left = 0;
//     CURLMsg *msg = NULL;
//     while ((msg = curl_multi_info_read(multi_handle, &msgs_left))) {
//         if (msg->msg == CURLMSG_DONE) {
//             CURL *eh = msg->easy_handle;
//             long task_idx = 0;
//             curl_easy_getinfo(eh, CURLINFO_PRIVATE, &task_idx);

//             /* Get all timing info - DIRECT from CURL, no calculations */
//             curl_easy_getinfo(eh, CURLINFO_NAMELOOKUP_TIME_T,
//                             (curl_off_t *)tasks[task_idx].namelookup_time);
//             curl_easy_getinfo(eh, CURLINFO_CONNECT_TIME_T,
//                             (curl_off_t *)tasks[task_idx].cnnt);
//             curl_easy_getinfo(eh, CURLINFO_APPCONNECT_TIME_T,
//                             (curl_off_t *)tasks[task_idx].appconnect_time);
//             curl_easy_getinfo(eh, CURLINFO_PRETRANSFER_TIME_T,
//                             (curl_off_t *)tasks[task_idx].pre_trans_time);
//             curl_easy_getinfo(eh, CURLINFO_STARTTRANSFER_TIME_T,
//                             (curl_off_t *)tasks[task_idx].start_trans_time);
//             curl_easy_getinfo(eh, CURLINFO_TOTAL_TIME_T,
//                             (curl_off_t *)tasks[task_idx].total_time);
//             curl_easy_getinfo(eh, CURLINFO_SPEED_DOWNLOAD_T,
//                             (curl_off_t *)tasks[task_idx].dls);
//             curl_easy_getinfo(eh, CURLINFO_SIZE_DOWNLOAD_T,
//                             (curl_off_t *)tasks[task_idx].size_dl);

//             /* Additional metrics */
//             curl_easy_getinfo(eh, CURLINFO_REDIRECT_TIME_T,
//                             (curl_off_t *)tasks[task_idx].redirect_time);
//             curl_easy_getinfo(eh, CURLINFO_HEADER_SIZE,
//                             (long *)tasks[task_idx].header_size);
//             curl_easy_getinfo(eh, CURLINFO_REDIRECT_COUNT,
//                             (long *)tasks[task_idx].redirect_count);

//             /* Check connection reuse */
//             long conn_id = 0;
//             curl_easy_getinfo(eh, CURLINFO_CONN_ID, &conn_id);
//             *(tasks[task_idx].connection_reused) = (conn_id > 0) ? 1 : 0;

//             /* Set status */
//             tasks[task_idx].status = (msg->data.result == CURLE_OK) ?
//                                     RET_SUCCESS : RET_FAIL;

//             if (msg->data.result != CURLE_OK) {
//                 fprintf(stderr, "Task %ld (tile %d) failed: %s\n", task_idx,
//                        tasks[task_idx].tile_id,
//                        curl_easy_strerror(msg->data.result));

//                 /* Debug info for HTTP/2 failures */
//                 if (msg->data.result == CURLE_HTTP2 ||
//                     msg->data.result == CURLE_HTTP2_STREAM) {
//                     fprintf(stderr, "  → HTTP/2 protocol error detected\n");
//                     fprintf(stderr, "  → Try checking server HTTP/2 support\n");
//                 } else if (msg->data.result == CURLE_COULDNT_CONNECT) {
//                     fprintf(stderr, "  → Connection refused - server may not be listening\n");
//                 } else if (msg->data.result == CURLE_SSL_CONNECT_ERROR) {
//                     fprintf(stderr, "  → SSL/TLS handshake failed\n");
//                 }
//             }
//         }
//     }

//     /* Cleanup */
//     for (int i = 0; i < num_tasks; i++) {
//         curl_multi_remove_handle(multi_handle, tasks[i].easy_handle);
//         curl_easy_cleanup(tasks[i].easy_handle);
//     }
//     curl_multi_cleanup(multi_handle);

//     return RET_SUCCESS;
// }

/*
 * http_pool.c
 *
 * Implements the CURL Multi-interface connection pool with two new features:
 *
 *  1. early_term_tau  — on every curl_multi_poll() wakeup, each active task's
 *     *prob_ptr is checked; if below tau, curl_multi_remove_handle() is called
 *     immediately, sending RESET_STREAM (HTTP/3) or RST_STREAM (HTTP/2) to
 *     reclaim kernel UDP-processing budget (Δstime) and TLS decode budget
 *     (Δutime).  Task is marked RET_EARLY_TERMINATED.
 *
 *  2. early_term_tau = 0.0f  — backward-compatible path: no probability check
 *     is performed, behaviour is identical to the original implementation.
 */

#include "proto_comp/http_pool.h"
#include <string.h>
#include <stdio.h>

/* ── lock stubs (single-threaded build) ────────────────────────────────── */
static void lock_cb(CURL *handle, curl_lock_data data,
                    curl_lock_access access, void *userptr)
{
    (void)handle;
    (void)data;
    (void)access;
    (void)userptr;
}

static void unlock_cb(CURL *handle, curl_lock_data data, void *userptr)
{
    (void)handle;
    (void)data;
    (void)userptr;
}

/* ── http_pool_init ─────────────────────────────────────────────────────── */
RET http_pool_init(http_pool_t *pool, HTTP_VERSION ver,
                   int max_parallel_downloads)
{
    if (!pool)
        return RET_FAIL;

    memset(pool, 0, sizeof(http_pool_t));

    pool->share_handle = curl_share_init();
    if (!pool->share_handle)
        return RET_FAIL;

    curl_share_setopt(pool->share_handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(pool->share_handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    curl_share_setopt(pool->share_handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    curl_share_setopt(pool->share_handle, CURLSHOPT_LOCKFUNC, lock_cb);
    curl_share_setopt(pool->share_handle, CURLSHOPT_UNLOCKFUNC, unlock_cb);

    pool->version = ver;
    pool->max_parallel_downloads = max_parallel_downloads;
    pool->early_term_tau = HTTP_POOL_TAU_DEFAULT;
    pool->is_initialized = 1;

    return RET_SUCCESS;
}

/* ── http_pool_destroy ──────────────────────────────────────────────────── */
RET http_pool_destroy(http_pool_t *pool)
{
    if (!pool || !pool->is_initialized)
        return RET_FAIL;
    if (pool->share_handle)
        curl_share_cleanup(pool->share_handle);
    memset(pool, 0, sizeof(http_pool_t));
    return RET_SUCCESS;
}

/* Parallel download using CURL Multi Interface (No Early Termination) */
RET http_pool_get_parallel(http_pool_t *pool,
                           download_task_t *tasks,
                           int num_tasks,
                           int max_concurrent)
{
    if (!pool || !pool->is_initialized || !tasks || num_tasks <= 0)
    {
        return RET_FAIL;
    }

    CURLM *multi_handle = curl_multi_init();
    if (!multi_handle)
    {
        return RET_FAIL;
    }

    /* Set max concurrent connections */
    curl_multi_setopt(multi_handle, CURLMOPT_MAXCONNECTS, (long)max_concurrent);
    curl_multi_setopt(multi_handle, CURLMOPT_MAX_HOST_CONNECTIONS, (long)max_concurrent);

    /* Enable HTTP/2 multiplexing (if available) */
    curl_multi_setopt(multi_handle, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);

    /* ── Create easy handles for each task ── */
    for (int i = 0; i < num_tasks; i++)
    {
        tasks[i].easy_handle = curl_easy_init();
        if (!tasks[i].easy_handle)
        {
            /* Cleanup on failure */
            for (int j = 0; j < i; j++)
            {
                curl_easy_cleanup(tasks[j].easy_handle);
            }
            curl_multi_cleanup(multi_handle);
            return RET_FAIL;
        }

        CURL *eh = tasks[i].easy_handle;

        /* Reset buffer */
        buffer_destroy(tasks[i].data);
        buffer_init(tasks[i].data);

        /* Copy common settings from pool */
        curl_easy_setopt(eh, CURLOPT_SHARE, pool->share_handle);
        curl_easy_setopt(eh, CURLOPT_BUFFERSIZE, 102400L);
        curl_easy_setopt(eh, CURLOPT_NOPROGRESS, 1L);
        curl_easy_setopt(eh, CURLOPT_USERAGENT, "curl/7.91.0");
        curl_easy_setopt(eh, CURLOPT_MAXREDIRS, 50L);
        curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(eh, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(eh, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(eh, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(eh, CURLOPT_FTP_SKIP_PASV_IP, 1L);

        /* Set HTTP version */
        switch (pool->version)
        {
        case STREAM_HTTP_1_1:
            curl_easy_setopt(eh, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
            break;
        case STREAM_HTTP_2_0:
            curl_easy_setopt(eh, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
            break;
        case STREAM_HTTP_3_0:
            curl_easy_setopt(eh, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_3ONLY);
            break;
        }

        /* Set specific URL and data for this task */
        curl_easy_setopt(eh, CURLOPT_URL, tasks[i].url);
        curl_easy_setopt(eh, CURLOPT_WRITEDATA, (void *)tasks[i].data);
        curl_easy_setopt(eh, CURLOPT_PRIVATE, (void *)(intptr_t)i); // Store task index

        /* Add to multi handle */
        curl_multi_add_handle(multi_handle, eh);
        
        /* Initialize status safely */
        tasks[i].status = RET_FAIL;
    }

    /* ── Perform parallel transfers (Strict loop, no interruptions) ── */
    int still_running = 0;
    do
    {
        CURLMcode mc = curl_multi_perform(multi_handle, &still_running);

        if (mc != CURLM_OK)
        {
            fprintf(stderr, "curl_multi_perform error: %s\n",
                    curl_multi_strerror(mc));
            break;
        }

        /* Wait for activity or timeout */
        if (still_running)
        {
            mc = curl_multi_poll(multi_handle, NULL, 0, 1000, NULL);
            if (mc != CURLM_OK)
            {
                fprintf(stderr, "curl_multi_poll error: %s\n",
                        curl_multi_strerror(mc));
                break;
            }
        }
    } while (still_running > 0);

    /* ── Check results for each transfer ── */
    int msgs_left = 0;
    CURLMsg *msg = NULL;
    while ((msg = curl_multi_info_read(multi_handle, &msgs_left)))
    {
        if (msg->msg == CURLMSG_DONE)
        {
            CURL *eh = msg->easy_handle;
            long task_idx = 0;
            curl_easy_getinfo(eh, CURLINFO_PRIVATE, &task_idx);
            
            if (task_idx < 0 || task_idx >= num_tasks) continue;

            /* Get all timing info - DIRECT from CURL, no calculations */
            curl_easy_getinfo(eh, CURLINFO_NAMELOOKUP_TIME_T,
                              (curl_off_t *)tasks[task_idx].namelookup_time);
            curl_easy_getinfo(eh, CURLINFO_CONNECT_TIME_T,
                              (curl_off_t *)tasks[task_idx].cnnt);
            curl_easy_getinfo(eh, CURLINFO_APPCONNECT_TIME_T,
                              (curl_off_t *)tasks[task_idx].appconnect_time);
            curl_easy_getinfo(eh, CURLINFO_PRETRANSFER_TIME_T,
                              (curl_off_t *)tasks[task_idx].pre_trans_time);
            curl_easy_getinfo(eh, CURLINFO_STARTTRANSFER_TIME_T,
                              (curl_off_t *)tasks[task_idx].start_trans_time);
            curl_easy_getinfo(eh, CURLINFO_TOTAL_TIME_T,
                              (curl_off_t *)tasks[task_idx].total_time);
            curl_easy_getinfo(eh, CURLINFO_SPEED_DOWNLOAD_T,
                              (curl_off_t *)tasks[task_idx].dls);
            curl_easy_getinfo(eh, CURLINFO_SIZE_DOWNLOAD_T,
                              (curl_off_t *)tasks[task_idx].size_dl);

            /* Additional metrics */
            curl_easy_getinfo(eh, CURLINFO_REDIRECT_TIME_T,
                              (curl_off_t *)tasks[task_idx].redirect_time);
            curl_easy_getinfo(eh, CURLINFO_HEADER_SIZE,
                              (long *)tasks[task_idx].header_size);
            curl_easy_getinfo(eh, CURLINFO_REDIRECT_COUNT,
                              (long *)tasks[task_idx].redirect_count);

            /* Check connection reuse */
            long conn_id = 0;
            curl_easy_getinfo(eh, CURLINFO_CONN_ID, &conn_id);
            *(tasks[task_idx].connection_reused) = (conn_id > 0) ? 1 : 0;

            /* Set completion status */
            tasks[task_idx].status = (msg->data.result == CURLE_OK) ? RET_SUCCESS : RET_FAIL;

            if (msg->data.result != CURLE_OK)
            {
                fprintf(stderr, "[http_pool] Task %ld (tile %d) failed: %s\n", task_idx,
                        tasks[task_idx].tile_id,
                        curl_easy_strerror(msg->data.result));

                /* Debug info for connection failures */
                if (msg->data.result == CURLE_HTTP2 ||
                    msg->data.result == CURLE_HTTP2_STREAM)
                {
                    fprintf(stderr, "  → HTTP/2 protocol error detected\n");
                }
                else if (msg->data.result == CURLE_COULDNT_CONNECT)
                {
                    fprintf(stderr, "  → Connection refused\n");
                }
                else if (msg->data.result == CURLE_SSL_CONNECT_ERROR)
                {
                    fprintf(stderr, "  → SSL/TLS handshake failed\n");
                }
            }
        }
    }

    /* ── Cleanup ── */
    for (int i = 0; i < num_tasks; i++)
    {
        if (tasks[i].easy_handle) 
        {
            curl_multi_remove_handle(multi_handle, tasks[i].easy_handle);
            curl_easy_cleanup(tasks[i].easy_handle);
            tasks[i].easy_handle = NULL;
        }
    }
    curl_multi_cleanup(multi_handle);

    return RET_SUCCESS;
}

RET http_pool_get_parallel_dynamic(http_pool_t *pool,
                                   download_task_t *tasks,
                                   int num_tasks,
                                   int max_concurrent,
                                   resource_monitor_t *rm)
{
    if (!pool || !pool->is_initialized || !tasks || num_tasks <= 0 || !rm)
        return RET_FAIL;

    CURLM *multi_handle = curl_multi_init();
    if (!multi_handle)
        return RET_FAIL;

    curl_multi_setopt(multi_handle, CURLMOPT_MAXCONNECTS, (long)max_concurrent);
    curl_multi_setopt(multi_handle, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);

    /* ── Init Curl Handles ── */
    for (int i = 0; i < num_tasks; i++)
    {
        tasks[i].easy_handle = curl_easy_init();
        if (!tasks[i].easy_handle)
            return RET_FAIL;

        CURL *eh = tasks[i].easy_handle;

        buffer_destroy(tasks[i].data);
        buffer_init(tasks[i].data);

        curl_easy_setopt(eh, CURLOPT_SHARE, pool->share_handle);
        curl_easy_setopt(eh, CURLOPT_BUFFERSIZE, 102400L);
        curl_easy_setopt(eh, CURLOPT_NOPROGRESS, 1L);
        curl_easy_setopt(eh, CURLOPT_USERAGENT, "curl/7.91.0");
        curl_easy_setopt(eh, CURLOPT_MAXREDIRS, 50L);
        curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(eh, CURLOPT_WRITEDATA, (void *)tasks[i].data);
        curl_easy_setopt(eh, CURLOPT_URL, tasks[i].url);

        curl_easy_setopt(eh, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(eh, CURLOPT_SSL_VERIFYHOST, 0L);

        curl_easy_setopt(eh, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(eh, CURLOPT_FTP_SKIP_PASV_IP, 1L);
        curl_easy_setopt(eh, CURLOPT_PRIVATE, (void *)(intptr_t)i);

        if (pool->version == STREAM_HTTP_3_0)
            curl_easy_setopt(eh, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_3ONLY);

        tasks[i].status = RET_FAIL;
        tasks[i].is_dispatched = 0;
    }

    /* ── Request tiles with high probability first ── */
    int still_running = 0;
    int active_bitmap[num_tasks]; // Manage overload status
    memset(active_bitmap, 0, sizeof(active_bitmap));

    for (int i = 0; i < num_tasks; i++)
    {
        float p = (tasks[i].prob_ptr) ? *(tasks[i].prob_ptr) : 0.0f;
        if (p >= 0.50f)
        {
            curl_multi_add_handle(multi_handle, tasks[i].easy_handle);
            tasks[i].is_dispatched = 1;
            active_bitmap[i] = 1;
            still_running++;
        }
    }

    /* ── POLLING with Dynamic Pacing ── */
    int stage_2_done = 0;
    do
    {
        CURLMcode mc = curl_multi_perform(multi_handle, &still_running);

        /* 1. PRIMARY LOGIC: Check CPU to send Stage 2 early if system is healthy */
        if (!stage_2_done)
        {
            resource_monitor_update(rm);
            float stress = get_system_status(rm);

            if (stress < 0.80f)
            { // If CPU load < 80%, admit Stage 2
                for (int i = 0; i < num_tasks; i++)
                {
                    if (!tasks[i].is_dispatched)
                    {
                        if (pool->early_term_tau > 0.0f && tasks[i].prob_ptr && *(tasks[i].prob_ptr) < pool->early_term_tau)
                        {
                            tasks[i].status = RET_EARLY_TERMINATED;
                            tasks[i].is_dispatched = 1;
                            continue;
                        }
                        curl_multi_add_handle(multi_handle, tasks[i].easy_handle);
                        tasks[i].is_dispatched = 1;
                        active_bitmap[i] = 1;
                        still_running++;
                    }
                }
                stage_2_done = 1;
            }
        }

        /* 2. THE FAIL-SAFE FIX: If CPU stayed high and Stage 1 finished, force-dispatch */
        if (still_running == 0 && !stage_2_done)
        {
            for (int i = 0; i < num_tasks; i++)
            {
                if (tasks[i].is_dispatched)
                    continue;

                if (pool->early_term_tau > 0.0f && tasks[i].prob_ptr && *(tasks[i].prob_ptr) < pool->early_term_tau)
                {
                    tasks[i].status = RET_EARLY_TERMINATED;
                    tasks[i].is_dispatched = 1;
                    continue;
                }

                curl_multi_add_handle(multi_handle, tasks[i].easy_handle);
                tasks[i].is_dispatched = 1;
                active_bitmap[i] = 1;
                still_running++;
            }
            stage_2_done = 1; /* never block here again */
        }

        /* 3. Early Termination Logic (Cancel active tiles if viewport changes) */
        if (pool->early_term_tau > 0.0f)
        {
            for (int i = 0; i < num_tasks; i++)
            {
                if (!active_bitmap[i] || !tasks[i].prob_ptr)
                    continue;

                float p = *(tasks[i].prob_ptr);
                if (p < pool->early_term_tau)
                {
                    fprintf(stderr,
                            "[http_pool] EARLY-TERM tile %d "
                            "(p=%.4f < tau=%.4f) — RESET_STREAM\n",
                            tasks[i].tile_id, p, pool->early_term_tau);

                    curl_off_t dl_size = 0;
                    curl_easy_getinfo(tasks[i].easy_handle, CURLINFO_SIZE_DOWNLOAD_T, &dl_size);
                    if (tasks[i].size_dl)
                        *(tasks[i].size_dl) = dl_size;

                    curl_multi_remove_handle(multi_handle, tasks[i].easy_handle);

                    curl_easy_cleanup(tasks[i].easy_handle);
                    tasks[i].easy_handle = NULL;

                    tasks[i].status = RET_EARLY_TERMINATED;
                    active_bitmap[i] = 0;

                    if (--still_running < 0)
                        still_running = 0;
                }
            }
        }

        if (still_running)
        {
            curl_multi_poll(multi_handle, NULL, 0, 10, NULL);
        }
    } while (still_running > 0 || !stage_2_done);

    /* ── BƯỚC 4: Collect data (HARVEST STATS) ── */
    /* ── harvest completion messages ─────────────────────────────────────── */
    int msgs_left = 0;
    CURLMsg *msg = NULL;
    while ((msg = curl_multi_info_read(multi_handle, &msgs_left)))
    {
        if (msg->msg != CURLMSG_DONE)
            continue;

        CURL *eh = msg->easy_handle;
        long task_idx = 0;
        curl_easy_getinfo(eh, CURLINFO_PRIVATE, &task_idx);
        if (task_idx < 0 || task_idx >= num_tasks)
            continue;

        curl_easy_getinfo(eh, CURLINFO_NAMELOOKUP_TIME_T,
                          (curl_off_t *)tasks[task_idx].namelookup_time);
        curl_easy_getinfo(eh, CURLINFO_CONNECT_TIME_T,
                          (curl_off_t *)tasks[task_idx].cnnt);
        curl_easy_getinfo(eh, CURLINFO_APPCONNECT_TIME_T,
                          (curl_off_t *)tasks[task_idx].appconnect_time);
        curl_easy_getinfo(eh, CURLINFO_PRETRANSFER_TIME_T,
                          (curl_off_t *)tasks[task_idx].pre_trans_time);
        curl_easy_getinfo(eh, CURLINFO_STARTTRANSFER_TIME_T,
                          (curl_off_t *)tasks[task_idx].start_trans_time);
        curl_easy_getinfo(eh, CURLINFO_TOTAL_TIME_T,
                          (curl_off_t *)tasks[task_idx].total_time);
        curl_easy_getinfo(eh, CURLINFO_SPEED_DOWNLOAD_T,
                          (curl_off_t *)tasks[task_idx].dls);
        curl_easy_getinfo(eh, CURLINFO_SIZE_DOWNLOAD_T,
                          (curl_off_t *)tasks[task_idx].size_dl);
        curl_easy_getinfo(eh, CURLINFO_REDIRECT_TIME_T,
                          (curl_off_t *)tasks[task_idx].redirect_time);
        curl_easy_getinfo(eh, CURLINFO_HEADER_SIZE,
                          (long *)tasks[task_idx].header_size);
        curl_easy_getinfo(eh, CURLINFO_REDIRECT_COUNT,
                          (long *)tasks[task_idx].redirect_count);

        long conn_id = 0;
        curl_easy_getinfo(eh, CURLINFO_CONN_ID, &conn_id);
        *(tasks[task_idx].connection_reused) = (conn_id > 0) ? 1 : 0;

        tasks[task_idx].status =
            (msg->data.result == CURLE_OK) ? RET_SUCCESS : RET_FAIL;

        if (msg->data.result != CURLE_OK)
        {
            fprintf(stderr, "[http_pool] task %ld (tile %d) failed: %s\n",
                    task_idx, tasks[task_idx].tile_id,
                    curl_easy_strerror(msg->data.result));
            if (msg->data.result == CURLE_HTTP2 ||
                msg->data.result == CURLE_HTTP2_STREAM)
                fprintf(stderr, "  → HTTP/2 protocol error\n");
            else if (msg->data.result == CURLE_COULDNT_CONNECT)
                fprintf(stderr, "  → Connection refused\n");
            else if (msg->data.result == CURLE_SSL_CONNECT_ERROR)
                fprintf(stderr, "  → SSL/TLS handshake failed\n");
        }
    }

    /* ── clean up ────────────────────────────────────────────────────────── */
    for (int i = 0; i < num_tasks; i++)
    {
        if (tasks[i].easy_handle)
        {
            curl_multi_remove_handle(multi_handle, tasks[i].easy_handle);
            curl_easy_cleanup(tasks[i].easy_handle);
            tasks[i].easy_handle = NULL;
        }
    }
    curl_multi_cleanup(multi_handle);
    return RET_SUCCESS;
}