#include <proto_comp/http.h>
#include <proto_comp/request_handler.h>
#include <string.h>

RET request_handler_init(request_handler_t *self,
                         const char        *ser_adrr,
                         COUNT              seg_count,
                         COUNT              version_count,
                         COUNT              tile_count)
{
  self->ser_addr      = ser_adrr;
  self->seg_count     = seg_count;
  self->tile_count    = tile_count;
  self->version_count = version_count;

  self->data = (buffer_t *)malloc(tile_count * sizeof(buffer_t));
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
    self->dls[i]              = 0;
    self->cnnt[i]             = 0;
    self->pre_trans_time[i]   = 0;
    self->start_trans_time[i] = 0;
    self->total_time[i]       = 0;
    self->size_dl[i]          = 0;
  }

  return RET_SUCCESS;
}

RET request_handler_post_get_info(request_handler_t *self,
                                  COUNT              chunk_id,
                                  int               *vp_tiles,
                                  int                num_vp_tiles,
                                  int               *chosen_versions)
{
  if (self == NULL)
  {
    return RET_FAIL;
  }

  char url_buffer[512];

  printf("=== CHUNK %lld DOWNLOAD STARTED ===\n", chunk_id);

  for (int i = 0; i < num_vp_tiles; i++)
  {
    int tile_id = vp_tiles[i];
    int version = chosen_versions[i];

    if (tile_id >= self->tile_count ||
        version >= self->version_count)
    {
      printf("Warning: Invalid tile_id %d or version %d\n",
             tile_id,
             version);
      continue;
    }

    int qp = tile_version_to_num(version);
    snprintf(
        url_buffer,
        sizeof(url_buffer),
        "%s/beach/beach_%lld/erp_8x6/tile_yuv/tile_%d_%d_480x360_QP%d.bin",
        self->ser_addr,
        chunk_id,
        tile_id / NO_OF_COLS,
        tile_id % NO_OF_COLS,
        qp);

    printf("\n[VIEWPORT] Tile %d (row %d, col %d) - Version %d "
           "(QP%d)\n",
           tile_id,
           tile_id / NO_OF_COLS,
           tile_id % NO_OF_COLS,
           version,
           qp);

    RET r = http_get_to_buffer(url_buffer,
                               STREAM_HTTP_2_0,
                               &self->data[tile_id],
                               &self->dls[tile_id],
                               &self->cnnt[tile_id],
                               &self->pre_trans_time[tile_id],
                               &self->start_trans_time[tile_id],
                               &self->total_time[tile_id],
                               &self->size_dl[tile_id]);

    printf("  Buffer size    : %zu bytes\n",
           self->data[tile_id].size);
    printf("  Download speed : %lld bps (%.2f Mbps)\n",
           self->dls[tile_id],
           self->dls[tile_id] / 1000000.0);
    printf("  Connect time   : %lld us (%.3f ms)\n",
           self->cnnt[tile_id],
           self->cnnt[tile_id] / 1000.0);
    printf("  Pre-transfer   : %lld us (%.3f ms)\n",
           self->pre_trans_time[tile_id],
           self->pre_trans_time[tile_id] / 1000.0);
    printf("  Start transfer : %lld us (%.3f ms)\n",
           self->start_trans_time[tile_id],
           self->start_trans_time[tile_id] / 1000.0);
    printf("  Total time     : %lld us (%.3f ms)\n",
           self->total_time[tile_id],
           self->total_time[tile_id] / 1000.0);
    printf("  Size downloaded: %lld bytes\n",
           self->size_dl[tile_id]);
    printf("  Status         : %s\n",
           (r == RET_SUCCESS) ? "SUCCESS" : "FAILED");

    if (r == RET_FAIL)
    {
      printf("  ERROR: Failed to download viewport tile %d\n",
             tile_id);
    }
  }

  printf("\n=== NON-VIEWPORT TILES ===\n");
  for (COUNT tile_id = 0; tile_id < self->tile_count; tile_id++)
  {
    bool is_viewport_tile = false;
    for (int i = 0; i < num_vp_tiles; i++)
    {
      if (vp_tiles[i] == tile_id)
      {
        is_viewport_tile = true;
        break;
      }
    }

    if (!is_viewport_tile)
    {
      snprintf(url_buffer,
               sizeof(url_buffer),
               "%s/beach/beach_%lld/erp_8x6/tile_yuv/"
               "tile_%d_%d_480x360_QP38.bin",
               self->ser_addr,
               chunk_id,
               tile_id / NO_OF_COLS,
               tile_id % NO_OF_COLS);

      printf("\n[NON-VP] Tile %lld (row %lld, col %lld) - QP38\n",
             tile_id,
             tile_id / NO_OF_COLS,
             tile_id % NO_OF_COLS);

      RET r = http_get_to_buffer(url_buffer,
                                 STREAM_HTTP_2_0,
                                 &self->data[tile_id],
                                 &self->dls[tile_id],
                                 &self->cnnt[tile_id],
                                 &self->pre_trans_time[tile_id],
                                 &self->start_trans_time[tile_id],
                                 &self->total_time[tile_id],
                                 &self->size_dl[tile_id]);

      printf("  Buffer size    : %zu bytes\n",
             self->data[tile_id].size);
      printf("  Download speed : %ld bps (%.2f Mbps)\n",
             self->dls[tile_id],
             self->dls[tile_id] / 1000000.0);
      printf("  Connect time   : %ld us (%.3f ms)\n",
             self->cnnt[tile_id],
             self->cnnt[tile_id] / 1000.0);
      printf("  Pre-transfer   : %ld us (%.3f ms)\n",
             self->pre_trans_time[tile_id],
             self->pre_trans_time[tile_id] / 1000.0);
      printf("  Start transfer : %ld us (%.3f ms)\n",
             self->start_trans_time[tile_id],
             self->start_trans_time[tile_id] / 1000.0);
      printf("  Total time     : %ld us (%.3f ms)\n",
             self->total_time[tile_id],
             self->total_time[tile_id] / 1000.0);
      printf("  Size downloaded: %ld bytes\n",
             self->size_dl[tile_id]);
      printf("  Status         : %s\n",
             (r == RET_SUCCESS) ? "SUCCESS" : "FAILED");

      if (r == RET_FAIL)
      {
        printf(
            "  ERROR: Failed to download non-viewport tile %lld\n",
            tile_id);
      }
    }
  }

  printf("\n=== CHUNK %lld SUMMARY ===\n", chunk_id);
  int64_t total_bytes = 0, total_time_sum = 0;
  int     successful_tiles = 0;

  for (COUNT i = 0; i < self->tile_count; i++)
  {
    if (self->size_dl[i] > 0)
    {
      total_bytes += self->size_dl[i];
      total_time_sum += self->total_time[i];
      successful_tiles++;
    }
  }

  if (successful_tiles > 0)
  {
    printf("Total tiles downloaded: %d\n", successful_tiles);
    printf("Total data size: %ld bytes (%.2f MB)\n",
           total_bytes,
           total_bytes / 1048576.0);
    printf("Average download time: %.3f ms\n",
           (total_time_sum / successful_tiles) / 1000.0);
    printf("Effective throughput: %.2f Mbps\n",
           (total_bytes * 8.0) / (total_time_sum / 1000000.0) /
               1000000.0);
  }

  printf("=== CHUNK %lld COMPLETED ===\n\n", chunk_id);

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
    
    self->dls[i]              = 0;
    self->cnnt[i]             = 0;
    self->pre_trans_time[i]   = 0;
    self->start_trans_time[i] = 0;
    self->total_time[i]       = 0;
    self->size_dl[i]          = 0;
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