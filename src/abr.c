#include "proto_comp/abr.h"
#include "proto_comp/bw_estimator.h"
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// double VIDEO_BIT_RATE[5] = {11839.0, 23066.0, 37100.0, 59496.0, 94589.0};
double VIDEO_BIT_RATE[5] = {94712.0, 184528.0, 296800.0, 475968.0, 756712.0};

RET abr_selector_init(abr_selector_t *abr_selector, int type)
{
  switch (type)
  {
  case ABR_FOR_NORMAL_BUF:
    abr_selector->choose_bitrate = abr_for_normal_buf;
    abr_selector->last_quality_default = NO_VIDEO_VERSION_NORMAL;
    break;
  case ABR_FOR_DANGER_BUF:
    abr_selector->choose_bitrate = abr_for_danger_buf;
    abr_selector->last_quality_default = NO_VIDEO_VERSION_DANGER;
    break;
  case ABR_FOR_HIGH_BUF:
    abr_selector->choose_bitrate = abr_for_high_buf;
    abr_selector->last_quality_default = NO_VIDEO_VERSION_HIGH;
    break;
  default:
    return RET_FAIL;
    break;
  }
  return RET_SUCCESS;
}

int abr_for_normal_buf(float bw_prediction,
                       int P,
                       float buffer_size,
                       int last_quality,
                       float *qoe)
{
  int total_combos = pow(NO_VIDEO_VERSION_NORMAL, P);
  int CHUNK_COMBO_OPTIONS[total_combos][P];

  for (int i = 0; i < total_combos; i++)
  {
    int reversed_i = total_combos - i - 1;
    int temp = reversed_i;
    for (int j = 0; j < P; j++)
    {
      CHUNK_COMBO_OPTIONS[i][j] = temp % NO_VIDEO_VERSION_NORMAL;
      temp /= NO_VIDEO_VERSION_NORMAL;
    }
  }

  int best_combo[5] = {0};
  float max_reward = -100000.0f;

  float bw_bytes_per_sec = bw_prediction;

  // #ifdef DEBUG_ABR
  // printf("[ABR] Normal Buffer Mode\n");
  // printf("  Bandwidth: %.2f bytes/sec = %.2f Mbps\n",
  //        bw_bytes_per_sec, bw_bytes_per_sec * 8.0 / 1000000.0);
  // printf("  Buffer: %.2f sec, Last Quality: %d\n", buffer_size, last_quality);
  // #endif
  const int MAX_QUALITY_JUMP = 2;

  for (int i = 0; i < total_combos; i++)
  {
    bool quality_diff_too_big = false;
    int combo_arr[P];
    memset(combo_arr, 0, sizeof(combo_arr));

    for (int j = 0; j < P; j++)
    {
      combo_arr[j] = CHUNK_COMBO_OPTIONS[i][j];
    }

    for (int j = 0; j < P - 1; j++)
    {
      if (abs(combo_arr[j] - combo_arr[j + 1]) > MAX_QUALITY_JUMP)
      {
        quality_diff_too_big = true;
        break;
      }
    }
    if (abs(combo_arr[0] - last_quality) > MAX_QUALITY_JUMP)
      quality_diff_too_big = true;
    if (quality_diff_too_big)
      continue;

    float curr_rebuff = 0.0f;
    float bitrate_sum = 0.0f;
    float smoothness = 0.0f;
    float curr_buffer = buffer_size;
    int temp_last_quality = last_quality;

    for (int j = 0; j < P; j++)
    {
      int chunk_quality = combo_arr[j];

      bitrate_sum += VIDEO_BIT_RATE[chunk_quality];

      float chunk_size_bytes = VIDEO_BIT_RATE[chunk_quality];
      float download_time = chunk_size_bytes / bw_bytes_per_sec;

      if (curr_buffer < download_time)
      {
        curr_rebuff += (download_time - curr_buffer);
        curr_buffer = 0;
      }
      else
      {
        curr_buffer -= download_time;
      }

      curr_buffer += SEGMENT_DURATION;
      if (curr_buffer > MAX_BUFFER_SIZE)
        curr_buffer = MAX_BUFFER_SIZE;

      smoothness += fabs(VIDEO_BIT_RATE[chunk_quality] -
                         VIDEO_BIT_RATE[temp_last_quality]);
      temp_last_quality = chunk_quality;
    }

    float bitrate_sum_kbits = (bitrate_sum * 8.0) / 1000.0;
    float rebuffer_penalty = curr_rebuff;
    float smoothness_kbits = (smoothness * 8.0) / 1000.0;

    float alpha_adjusted = 0.8f; // Reduced bitrate reward
    float beta_adjusted = 2.5f;  // Increased rebuffer penalty
    float theta_adjusted = 0.6f; // Reduced smoothness weight

    float reward = alpha_adjusted * bitrate_sum_kbits -
                   beta_adjusted * rebuffer_penalty * 1000.0 -
                   theta_adjusted * smoothness_kbits;

    if (reward > max_reward)
    {
      for (int k = 0; k < P; k++)
        best_combo[k] = CHUNK_COMBO_OPTIONS[i][k];
      max_reward = reward;
    }
  }

  // #ifdef DEBUG_ABR
  // printf("  Selected Quality: %d (QP%d, %.2f KB)\n",
  //        best_combo[0], tile_version_to_num(best_combo[0]),
  //        VIDEO_BIT_RATE[best_combo[0]] / 1024.0);
  // printf("  Max Reward: %.2f\n", max_reward);
  // #endif
  *qoe = max_reward;
  return best_combo[0];
}

int abr_for_danger_buf(float bw_prediction,
                       int P,
                       float buffer_size,
                       int last_quality,
                       float *qoe)
{
  int total_combos = pow(NO_VIDEO_VERSION_DANGER, P);
  int CHUNK_COMBO_OPTIONS[total_combos][P];

  for (int i = 0; i < total_combos; i++)
  {
    int temp = i;
    for (int j = 0; j < P; j++)
    {
      CHUNK_COMBO_OPTIONS[i][j] = temp % NO_VIDEO_VERSION_DANGER;
      temp /= NO_VIDEO_VERSION_DANGER;
    }
  }

  int best_combo[5] = {0};
  float max_reward = -100000.0f;

  float bw_bytes_per_sec = bw_prediction;

  // #ifdef DEBUG_ABR
  // printf("[ABR] DANGER Buffer Mode\n");
  // printf("  Bandwidth: %.2f bytes/sec = %.2f Mbps\n",
  //        bw_bytes_per_sec, bw_bytes_per_sec * 8.0 / 1000000.0);
  // printf("  Buffer: %.2f sec (CRITICAL!), Last Quality: %d\n",
  //        buffer_size, last_quality);
  // #endif

  for (int i = 0; i < total_combos; i++)
  {
    int combo_arr[P];
    memset(combo_arr, 0, sizeof(combo_arr));

    for (int j = 0; j < P; j++)
    {
      combo_arr[j] = CHUNK_COMBO_OPTIONS[i][j];
    }

    float curr_rebuff = 0.0f;
    float bitrate_sum = 0.0f;
    float smoothness = 0.0f;
    float curr_buffer = buffer_size;
    int temp_last_quality = last_quality;

    for (int j = 0; j < P; j++)
    {
      int chunk_quality = combo_arr[j];
      bitrate_sum += VIDEO_BIT_RATE[chunk_quality];

      float chunk_size_bytes = VIDEO_BIT_RATE[chunk_quality];
      float download_time = chunk_size_bytes / bw_bytes_per_sec;

      if (curr_buffer < download_time)
      {
        curr_rebuff += (download_time - curr_buffer);
        curr_buffer = 0;
      }
      else
      {
        curr_buffer -= download_time;
      }

      curr_buffer += SEGMENT_DURATION;
      if (curr_buffer > MAX_BUFFER_SIZE)
        curr_buffer = MAX_BUFFER_SIZE;

      smoothness += fabs(VIDEO_BIT_RATE[chunk_quality] -
                         VIDEO_BIT_RATE[temp_last_quality]);
      temp_last_quality = chunk_quality;
    }

    float bitrate_sum_kbits = (bitrate_sum * 8.0) / 1000.0;
    float rebuffer_penalty = curr_rebuff;
    float smoothness_kbits = (smoothness * 8.0) / 1000.0;


    float alpha_adjusted = 0.5f; // Low bitrate reward
    float beta_adjusted = 4.0f;  // Very high rebuffer penalty
    float theta_adjusted = 0.3f; // Low smoothness weight

    float reward = alpha_adjusted * bitrate_sum_kbits -
                   beta_adjusted * rebuffer_penalty * 1000.0 -
                   theta_adjusted * smoothness_kbits;

    if (reward > max_reward)
    {
      for (int k = 0; k < P; k++)
        best_combo[k] = CHUNK_COMBO_OPTIONS[i][k];
      max_reward = reward;
    }
  }

  // #ifdef DEBUG_ABR
  // printf("  Selected Quality: %d (QP%d, %.2f KB) - Conservative\n",
  //        best_combo[0], tile_version_to_num(best_combo[0]),
  //        VIDEO_BIT_RATE[best_combo[0]] / 1024.0);
  // #endif
  *qoe = max_reward;
  return best_combo[0];
}

int abr_for_high_buf(float bw_prediction,
                     int P,
                     float buffer_size,
                     int last_quality,
                     float *qoe)
{
  int total_combos = pow(NO_VIDEO_VERSION_HIGH, P);
  int CHUNK_COMBO_OPTIONS[total_combos][P];

  for (int i = 0; i < total_combos; i++)
  {
    int reversed_i = total_combos - i - 1;
    int temp = reversed_i;
    for (int j = 0; j < P; j++)
    {
      CHUNK_COMBO_OPTIONS[i][j] = temp % NO_VIDEO_VERSION_HIGH;
      temp /= NO_VIDEO_VERSION_HIGH;
    }
  }

  int best_combo[5] = {0};
  float max_reward = -100000.0f;

  float bw_bytes_per_sec = bw_prediction;
  const int MAX_QUALITY_JUMP = 2;

  // #ifdef DEBUG_ABR
  // printf("[ABR] HIGH Buffer Mode\n");
  // printf("  Bandwidth: %.2f bytes/sec = %.2f Mbps\n",
  //        bw_bytes_per_sec, bw_bytes_per_sec * 8.0 / 1000000.0);
  // printf("  Buffer: %.2f sec (SAFE), Last Quality: %d\n",
  //        buffer_size, last_quality);
  // #endif

  for (int i = 0; i < total_combos; i++)
  {
    bool quality_diff_too_big = false;
    int combo_arr[P];
    memset(combo_arr, 0, sizeof(combo_arr));

    for (int j = 0; j < P; j++)
    {
      combo_arr[j] = CHUNK_COMBO_OPTIONS[i][j];
    }

    for (int j = 0; j < P - 1; j++)
    {
      if (abs(combo_arr[j] - combo_arr[j + 1]) > MAX_QUALITY_JUMP)
      {
        quality_diff_too_big = true;
        break;
      }
    }
    if (abs(combo_arr[0] - last_quality) > MAX_QUALITY_JUMP)
      quality_diff_too_big = true;
    if (quality_diff_too_big)
      continue;

    float curr_rebuff = 0.0f;
    float bitrate_sum = 0.0f;
    float smoothness = 0.0f;
    float curr_buffer = buffer_size;
    int temp_last_quality = last_quality;

    for (int j = 0; j < P; j++)
    {
      int chunk_quality = combo_arr[j];
      bitrate_sum += VIDEO_BIT_RATE[chunk_quality];

      float chunk_size_bytes = VIDEO_BIT_RATE[chunk_quality];
      float download_time = chunk_size_bytes / bw_bytes_per_sec;

      if (curr_buffer < download_time)
      {
        curr_rebuff += (download_time - curr_buffer);
        curr_buffer = 0;
      }
      else
      {
        curr_buffer -= download_time;
      }

      curr_buffer += SEGMENT_DURATION;
      if (curr_buffer > MAX_BUFFER_SIZE)
        curr_buffer = MAX_BUFFER_SIZE;

      smoothness += fabs(VIDEO_BIT_RATE[chunk_quality] -
                         VIDEO_BIT_RATE[temp_last_quality]);
      temp_last_quality = chunk_quality;
    }

    float bitrate_sum_kbits = (bitrate_sum * 8.0) / 1000.0;
    float rebuffer_penalty = curr_rebuff;
    float smoothness_kbits = (smoothness * 8.0) / 1000.0;
    
    float alpha_adjusted = 1.2f; // High bitrate reward
    float beta_adjusted = 1.5f;  // Low rebuffer penalty (we have buffer)
    float theta_adjusted = 0.8f; // Moderate smoothness

    float reward = alpha_adjusted * bitrate_sum_kbits -
                   beta_adjusted * rebuffer_penalty * 1000.0 -
                   theta_adjusted * smoothness_kbits;

    if (reward > max_reward)
    {
      for (int k = 0; k < P; k++)
        best_combo[k] = CHUNK_COMBO_OPTIONS[i][k];
      max_reward = reward;
    }
  }

  // #ifdef DEBUG_ABR
  // printf("  Selected Quality: %d (QP%d, %.2f KB) - Aggressive\n",
  //        best_combo[0], tile_version_to_num(best_combo[0]),
  //        VIDEO_BIT_RATE[best_combo[0]] / 1024.0);
  // #endif
  *qoe = max_reward;
  return best_combo[0];
}