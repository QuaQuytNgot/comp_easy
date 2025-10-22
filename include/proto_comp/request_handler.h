#ifndef REQUEST_HANDLER_H
#define REQUEST_HANDLER_H

#include "buffer.h"
#include "define.h"

// typedef request_handler_t request_handler_t;

typedef struct request_handler_t
{
  char         *ser_addr;
  COUNT         seg_count;
  COUNT         tile_count;
  COUNT         version_count;

  buffer_t     *data;
  bw_t         *dls;
  count_time_t *cnnt;
  count_time_t *pre_trans_time;
  count_time_t *start_trans_time;
  count_time_t *total_time;
  count_time_t *size_dl;
} request_handler_t;

RET request_handler_init(request_handler_t *self,
                         const char        *ser_adrr,
                         COUNT              seg_count,
                         COUNT              version_count,
                         COUNT              tile_count);

RET request_handler_post_get_info(request_handler_t *self,
                                  COUNT              chunk_id,
                                  int               *vp_tiles,
                                  int                num_vp_tiles,
                                  int               *chosen_versions);
//should add one more argument to take dl speed for bw_estimation
//same for time downloading

RET request_handler_reset(request_handler_t *self);

RET request_handler_destroy(request_handler_t *self);

int tile_version_to_num(int version);

#endif
