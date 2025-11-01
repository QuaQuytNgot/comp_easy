#ifndef DEFINE_H
#define DEFINE_H

#include <curl/curl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>

typedef unsigned long long COUNT;
typedef uint64_t bw_t;
typedef uint64_t count_t;
typedef int64_t count_time_t;

#define RET         unsigned int
#define RET_FAIL    0
#define RET_SUCCESS 1

#define NULL_POINTER ((void *)0)

typedef enum
{
  STREAM_HTTP_1_1 = 1,
  STREAM_HTTP_2_0,
  STREAM_HTTP_3_0
} HTTP_VERSION;

#define BW                long int // bytes/s
#define BYTE              unsigned int
#define BW_DEFAULT        (-1)
#define HISTORY_SIZE      100
#define PREDICTION_WINDOW 3

#define MAX_BUFFER_SIZE   4.0f
#define STEP              0.01f
#define SEGMENT_DURATION  1.0f
#define B_MIN             1.0f
#define B_NORMAL          2.0f
#define B_HIGH            3.0f
#define VIEWPORT_WIDTH_DEGREES 90.0f
#define VIEWPORT_HEIGHT_DEGREES 90.0f
#define TILE_WIDTH (360.0f / NO_OF_COLS)  // 90 degrees
#define TILE_HEIGHT (180.0f / NO_OF_ROWS)  // 30 degrees
#define MAX_PARALLEL_VP_TILES 8
#define MAX_PARALLEL_NON_VP_TILES 4

typedef enum
{
  BW_ESTIMATOR_HARMONIC = 1
} BW_ESTIMATOR;

typedef enum
{
  VIEWPORT_ESTIMATOR_LEGR = 1
} VIEWPORT_ESTIMATOR;

typedef enum
{
  DYNAMIC_TILE_SELECTION = 1,
  FIXED_TILE_SELECTION
} TILE_SELECTION;

typedef enum
{
  ABR_FOR_NORMAL_BUF = 1,
  ABR_FOR_DANGER_BUF,
  ABR_FOR_HIGH_BUF
} ABR_SCHEME;

#define NO_VIDEO_VERSION_NORMAL 5
#define NO_VIDEO_VERSION_DANGER 3
#define NO_VIDEO_VERSION_HIGH   4
#define alpha                   1
#define beta                    1.85
#define theta                   1

#define roll   0
#define W      3840
#define H      2160
#define FOV_h  90
#define FOV_v  90

#define NO_OF_ROWS 6
#define NO_OF_COLS 8

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

#endif 
