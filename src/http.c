#include <proto_comp/buffer.h>
#include <proto_comp/define.h>
#include <proto_comp/http.h>
#include <string.h>

size_t
write_callback(void *data, size_t size, size_t nmemb, void *userdata)
{
  size_t realsize = 0;
  buffer_t *buf = NULL_POINTER;
  char *ptr = NULL_POINTER;

  realsize = size * nmemb;
  buf = (buffer_t *)userdata;

  ptr = realloc(buf->data, buf->size + realsize + 1);
  if (ptr == NULL)
  {
    return 0;
  }
  buf->data = ptr;
  memcpy(&(buf->data[buf->size]), data, realsize);
  buf->size += realsize;
  buf->data[buf->size] = '\0';

  return realsize;
}

RET http_get_to_buffer(const char *url,
                       HTTP_VERSION ver,
                       buffer_t *data,
                       bw_t *dls,
                       count_time_t *cnnt,
                       count_time_t *pre_trans_time,
                       count_time_t *start_trans_time,
                       count_time_t *total_time,
                       count_time_t *size_dl,
                       // NEW PARAMETERS:
                       count_time_t *namelookup_time,
                       count_time_t *appconnect_time,
                       int *connection_reused)
{
  CURL *curl = curl_easy_init();
  CURLcode res = 0;

  if (!curl)
  {
    return RET_FAIL;
  }

  curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 102400L);
  curl_easy_setopt(curl, CURLOPT_URL, url);

  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "curl/7.91.0");
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 50L);

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)data);

  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

  switch (ver)
  {
  case STREAM_HTTP_1_1:
    curl_easy_setopt(curl, CURLOPT_FTP_SKIP_PASV_IP, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(
        curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    break;

  case STREAM_HTTP_2_0:
    curl_easy_setopt(curl, CURLOPT_FTP_SKIP_PASV_IP, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(
        curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
    break;

  case STREAM_HTTP_3_0:
    curl_easy_setopt(curl, CURLOPT_FTP_SKIP_PASV_IP, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(
        curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_3ONLY);
    // curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
    // (long)CURL_HTTP_VERSION_3ONLY);
    break;
  default:
    return RET_FAIL;
    break;
  }

  res = curl_easy_perform(curl);

  if (dls != NULL_POINTER)
  {
    // Existing metrics
    curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME_T, (curl_off_t *)cnnt);
    curl_easy_getinfo(curl, CURLINFO_PRETRANSFER_TIME_T, (curl_off_t *)pre_trans_time);
    curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME_T, (curl_off_t *)start_trans_time);
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, (curl_off_t *)total_time);
    curl_easy_getinfo(curl, CURLINFO_SPEED_DOWNLOAD_T, (curl_off_t *)dls);
    curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, (curl_off_t *)size_dl);

    // NEW METRICS:
    if (namelookup_time != NULL_POINTER)
    {
      curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME_T, (curl_off_t *)namelookup_time);
    }

    if (appconnect_time != NULL_POINTER)
    {
      curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME_T, (curl_off_t *)appconnect_time);
    }

    if (connection_reused != NULL_POINTER)
    {
      long reused = 0;
      curl_easy_getinfo(curl, CURLINFO_CONN_ID, &reused);
      *connection_reused = (reused > 0) ? 1 : 0;
    }
  }

  curl_easy_cleanup(curl);

  if (res != CURLE_OK)
  {
    buffer_destroy(data);
    return RET_FAIL;
  }

  return RET_SUCCESS;
}