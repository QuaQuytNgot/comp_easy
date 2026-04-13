#ifndef ABR_H
#define ABR_H

#include "define.h"

typedef struct abr_selector_t abr_selector_t;

extern double VIDEO_BIT_RATE[5];

struct abr_selector_t
{
  int last_quality_default;
  int (*choose_bitrate)(float bw_prediction_bps, int P, float buffer_size, int last_quality, float *qoe, int num_vp_tiles, int total_tiles);
};

RET abr_selector_init(abr_selector_t *abr_selector, int type);

int abr_for_normal_buf(float bw_prediction_bps, int P, float buffer_size, int last_quality, float *qoe, int num_vp_tiles, int total_tiles);

int abr_for_danger_buf(float bw_prediction_bps, int P, float buffer_size, int last_quality, float *qoe, int num_vp_tiles, int total_tiles);

int abr_for_high_buf(float bw_prediction_bps, int P, float buffer_size, int last_quality, float *qoe, int num_vp_tiles, int total_tiles);

#endif