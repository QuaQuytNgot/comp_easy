#include "proto_comp/http_pool.h"
#include <string.h>
#include <stdio.h>

/* Lock callback for thread-safe sharing */
static void lock_cb(CURL *handle, curl_lock_data data,
                   curl_lock_access access, void *userptr) {
    (void)handle;
    (void)data;
    (void)access;
    (void)userptr;
    /* In production, use actual mutex locks here */
}

static void unlock_cb(CURL *handle, curl_lock_data data, void *userptr) {
    (void)handle;
    (void)data;
    (void)userptr;
    /* In production, use actual mutex unlocks here */
}

RET http_pool_init(http_pool_t *pool, HTTP_VERSION ver) {
    if (!pool) return RET_FAIL;
    
    memset(pool, 0, sizeof(http_pool_t));
    
    /* Create share handle for connection reuse */
    pool->share_handle = curl_share_init();
    if (!pool->share_handle) {
        return RET_FAIL;
    }
    
    /* Share DNS cache, SSL sessions, and connections */
    curl_share_setopt(pool->share_handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(pool->share_handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    curl_share_setopt(pool->share_handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    
    /* Set lock functions (for thread safety) */
    curl_share_setopt(pool->share_handle, CURLSHOPT_LOCKFUNC, lock_cb);
    curl_share_setopt(pool->share_handle, CURLSHOPT_UNLOCKFUNC, unlock_cb);
    
    /* Create reusable CURL handle */
    pool->curl_handle = curl_easy_init();
    if (!pool->curl_handle) {
        curl_share_cleanup(pool->share_handle);
        return RET_FAIL;
    }
    
    /* Set common options once */
    curl_easy_setopt(pool->curl_handle, CURLOPT_SHARE, pool->share_handle);
    curl_easy_setopt(pool->curl_handle, CURLOPT_BUFFERSIZE, 102400L);
    curl_easy_setopt(pool->curl_handle, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(pool->curl_handle, CURLOPT_USERAGENT, "curl/7.91.0");
    curl_easy_setopt(pool->curl_handle, CURLOPT_MAXREDIRS, 50L);
    curl_easy_setopt(pool->curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(pool->curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(pool->curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(pool->curl_handle, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(pool->curl_handle, CURLOPT_FTP_SKIP_PASV_IP, 1L);
    
    /* Set HTTP version */
    pool->version = ver;
    switch (ver) {
        case STREAM_HTTP_1_1:
            curl_easy_setopt(pool->curl_handle, CURLOPT_HTTP_VERSION, 
                           CURL_HTTP_VERSION_1_1);
            break;
        case STREAM_HTTP_2_0:
            curl_easy_setopt(pool->curl_handle, CURLOPT_HTTP_VERSION, 
                           CURL_HTTP_VERSION_2_0);
            break;
        case STREAM_HTTP_3_0:
            curl_easy_setopt(pool->curl_handle, CURLOPT_HTTP_VERSION, 
                           CURL_HTTP_VERSION_3ONLY);
            break;
        default:
            curl_easy_cleanup(pool->curl_handle);
            curl_share_cleanup(pool->share_handle);
            return RET_FAIL;
    }
    
    pool->is_initialized = 1;
    return RET_SUCCESS;
}

RET http_pool_destroy(http_pool_t *pool) {
    if (!pool || !pool->is_initialized) return RET_FAIL;
    
    if (pool->curl_handle) {
        curl_easy_cleanup(pool->curl_handle);
    }
    if (pool->share_handle) {
        curl_share_cleanup(pool->share_handle);
    }
    
    memset(pool, 0, sizeof(http_pool_t));
    return RET_SUCCESS;
}

RET http_pool_get(http_pool_t *pool,
                  const char *url,
                  buffer_t *data,
                  bw_t *dls,
                  count_time_t *cnnt,
                  count_time_t *pre_trans_time,
                  count_time_t *start_trans_time,
                  count_time_t *total_time,
                  count_time_t *size_dl) {
    
    if (!pool || !pool->is_initialized || !url || !data) {
        return RET_FAIL;
    }
    
    CURL *curl = pool->curl_handle;
    
    /* Reset buffer for this request */
    buffer_destroy(data);
    buffer_init(data);
    
    /* Set URL and write data */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)data);
    
    /* Perform request */
    CURLcode res = curl_easy_perform(curl);
    
    /* Get timing information */
    if (dls != NULL_POINTER) {
        curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME_T, (curl_off_t *)cnnt);
        curl_easy_getinfo(curl, CURLINFO_PRETRANSFER_TIME_T, 
                         (curl_off_t *)pre_trans_time);
        curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME_T, 
                         (curl_off_t *)start_trans_time);
        curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, (curl_off_t *)total_time);
        curl_easy_getinfo(curl, CURLINFO_SPEED_DOWNLOAD_T, (curl_off_t *)dls);
        curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, (curl_off_t *)size_dl);
    }
    
    if (res != CURLE_OK) {
        buffer_destroy(data);
        return RET_FAIL;
    }
    
    return RET_SUCCESS;
}

/* Parallel download using CURL Multi Interface */
RET http_pool_get_parallel(http_pool_t *pool,
                           download_task_t *tasks,
                           int num_tasks,
                           int max_concurrent) {
    
    if (!pool || !pool->is_initialized || !tasks || num_tasks <= 0) {
        return RET_FAIL;
    }
    
    CURLM *multi_handle = curl_multi_init();
    if (!multi_handle) {
        return RET_FAIL;
    }
    
    /* Set max concurrent connections */
    curl_multi_setopt(multi_handle, CURLMOPT_MAXCONNECTS, max_concurrent);
    curl_multi_setopt(multi_handle, CURLMOPT_MAX_HOST_CONNECTIONS, max_concurrent);
    
    /* Create easy handles for each task */
    for (int i = 0; i < num_tasks; i++) {
        tasks[i].easy_handle = curl_easy_init();
        if (!tasks[i].easy_handle) {
            /* Cleanup on failure */
            for (int j = 0; j < i; j++) {
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
        
        /* Set HTTP version */
        switch (pool->version) {
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
    }
    
    /* Perform parallel transfers */
    int still_running = 0;
    do {
        CURLMcode mc = curl_multi_perform(multi_handle, &still_running);
        
        if (mc != CURLM_OK) {
            fprintf(stderr, "curl_multi_perform error: %s\n", 
                   curl_multi_strerror(mc));
            break;
        }
        
        /* Wait for activity or timeout */
        if (still_running) {
            mc = curl_multi_poll(multi_handle, NULL, 0, 1000, NULL);
            if (mc != CURLM_OK) {
                fprintf(stderr, "curl_multi_poll error: %s\n", 
                       curl_multi_strerror(mc));
                break;
            }
        }
    } while (still_running);
    
    /* Check results for each transfer */
    int msgs_left = 0;
    CURLMsg *msg = NULL;
    while ((msg = curl_multi_info_read(multi_handle, &msgs_left))) {
        if (msg->msg == CURLMSG_DONE) {
            CURL *eh = msg->easy_handle;
            int task_idx = 0;
            curl_easy_getinfo(eh, CURLINFO_PRIVATE, (char **)&task_idx);
            
            /* Get timing info */
            if (tasks[task_idx].dls != NULL_POINTER) {
                curl_easy_getinfo(eh, CURLINFO_CONNECT_TIME_T, 
                                (curl_off_t *)tasks[task_idx].cnnt);
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
            }
            
            /* Set status */
            tasks[task_idx].status = (msg->data.result == CURLE_OK) ? 
                                    RET_SUCCESS : RET_FAIL;
            
            if (msg->data.result != CURLE_OK) {
                fprintf(stderr, "Task %d failed: %s\n", task_idx,
                       curl_easy_strerror(msg->data.result));
            }
        }
    }
    
    /* Cleanup */
    for (int i = 0; i < num_tasks; i++) {
        curl_multi_remove_handle(multi_handle, tasks[i].easy_handle);
        curl_easy_cleanup(tasks[i].easy_handle);
    }
    curl_multi_cleanup(multi_handle);
    
    return RET_SUCCESS;
}
