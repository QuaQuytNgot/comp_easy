#ifndef HTTP_H

#include "define.h"
#include <curl/curl.h>
#include <stddef.h>
#include <stdlib.h>
#include "buffer.h"

size_t
write_callback(void *ptr, size_t size, size_t nmemb, void *userdata);

RET http_get_to_buffer(const char   *url,
                       HTTP_VERSION  ver,
                       buffer_t     *data,
                       bw_t         *dls,
                       count_time_t *cnnt,
                       count_time_t *pre_trans_time,
                       count_time_t *start_trans_time,
                       count_time_t *total_time,
                       count_time_t *size_dl,
                       // NEW PARAMETERS:
                       count_time_t *namelookup_time,
                       count_time_t *appconnect_time,
                       int          *connection_reused);

#endif