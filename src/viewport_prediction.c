#include "proto_comp/viewport_prediction.h"
#include <stdlib.h>
#include <string.h>

RET vpes_init(viewport_prediction_t *vpes,
                             float                 *yaw_history,
                             float                 *pitch_history,
                             int                   *timestamps,
                             int                    current_index,
                             int                    sample_count,
                             int                    next_timestamp,
                             int                    history_size,
                             VIEWPORT_ESTIMATOR     type)
{

  if (vpes == NULL_POINTER)
  {
    return RET_FAIL;
  }

  vpes->yaw_history    = yaw_history;
  vpes->pitch_history  = pitch_history;
  vpes->timestamps     = timestamps;
  vpes->current_index  = current_index;
  vpes->sample_count   = sample_count;
  vpes->next_timestamp = next_timestamp;
  vpes->history_size   = history_size;

  vpes->kalman.initialized = 0;

  switch (type)
  {
  case VIEWPORT_ESTIMATOR_LEGR:
    vpes->vpes_post = vpes_legr;
    break;

  case VIEWPORT_ESTIMATOR_KALMAN:
    vpes->vpes_post = vpes_kalman;
    break;

  default:
    return RET_FAIL;
    break;
  }
  return RET_SUCCESS;
}

RET calculate_linear_regression(float *y_values,
                                int   *x_values,
                                int    n,
                                float *slope,
                                float *intercept)
{
  if (n < 2 || y_values == NULL_POINTER ||
      x_values == NULL_POINTER || slope == NULL_POINTER ||
      intercept == NULL_POINTER)
  {
    return RET_FAIL;
  }

  float sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0;

  for (int i = 0; i < n; i++)
  {
    sum_x += x_values[i];
    sum_y += y_values[i];
    sum_xy += x_values[i] * y_values[i];
    sum_x2 += x_values[i] * x_values[i];
  }

  float denominator = n * sum_x2 - sum_x * sum_x;
  if (fabs(denominator) < 1e-10)
  {
    return RET_FAIL;
  }

  *slope     = (n * sum_xy - sum_x * sum_y) / denominator;
  *intercept = (sum_y - (*slope) * sum_x) / n;

  return RET_SUCCESS;
}

float wrap_angle_360(float angle)
{
  while (angle < 0)
    angle += 360;
  while (angle >= 360)
    angle -= 360;
  return angle;
}

float clamp_pitch(float pitch)
{
  if (pitch > 90)
    return 90;
  if (pitch < -90)
    return -90;
  return pitch;
}

void adjust_yaw_for_wrapping(float *yaw_values, int n)
{
  if (n < 2 || yaw_values == NULL_POINTER)
    return;

  float min_yaw = yaw_values[0], max_yaw = yaw_values[0];
  for (int i = 1; i < n; i++)
  {
    if (yaw_values[i] < min_yaw)
      min_yaw = yaw_values[i];
    if (yaw_values[i] > max_yaw)
      max_yaw = yaw_values[i];
  }

  if (max_yaw - min_yaw > 180)
  {
    for (int i = 0; i < n; i++)
    {
      if (yaw_values[i] < 180)
      {
        yaw_values[i] += 360;
      }
    }
  }
}

void add_viewport_sample(viewport_prediction_t *vpes,
                         float                  yaw,
                         float                  pitch)
{
  if (vpes == NULL_POINTER || vpes->yaw_history == NULL_POINTER ||
      vpes->pitch_history == NULL_POINTER ||
      vpes->timestamps == NULL_POINTER)
  {
    return;
  }

  vpes->yaw_history[vpes->current_index]   = yaw;
  vpes->pitch_history[vpes->current_index] = pitch;
  vpes->timestamps[vpes->current_index]    = vpes->next_timestamp++;

  vpes->current_index =
      (vpes->current_index + 1) % vpes->history_size;
  if (vpes->sample_count < vpes->history_size)
  {
    vpes->sample_count++;
  }
}

RET vpes_legr(viewport_prediction_t *vpes, float *yaw, float *pitch)
{
  if (vpes == NULL_POINTER || yaw == NULL_POINTER ||
      pitch == NULL_POINTER)
  {
    return RET_FAIL;
  }

  float *yaw_work =
      (float *)malloc(sizeof(float) * vpes->sample_count);
  float *pitch_work =
      (float *)malloc(sizeof(float) * vpes->sample_count);
  int *time_work = (int *)malloc(sizeof(int) * vpes->sample_count);

  if (yaw_work == NULL_POINTER || pitch_work == NULL_POINTER ||
      time_work == NULL_POINTER)
  {
    free(yaw_work);
    free(pitch_work);
    free(time_work);
    return RET_FAIL;
  }

  int samples_to_use = vpes->sample_count;
  int start_idx =
      (vpes->current_index - samples_to_use + vpes->history_size) %
      vpes->history_size;

  for (int i = 0; i < samples_to_use; i++)
  {
    int idx       = (start_idx + i) % vpes->history_size;
    yaw_work[i]   = vpes->yaw_history[idx];
    pitch_work[i] = vpes->pitch_history[idx];
    time_work[i]  = vpes->timestamps[idx];
  }

  adjust_yaw_for_wrapping(yaw_work, samples_to_use);

  float yaw_slope, yaw_intercept;
  float pitch_slope, pitch_intercept;

  RET   yaw_result   = calculate_linear_regression(yaw_work,
                                               time_work,
                                               samples_to_use,
                                               &yaw_slope,
                                               &yaw_intercept);
  RET   pitch_result = calculate_linear_regression(pitch_work,
                                                 time_work,
                                                 samples_to_use,
                                                 &pitch_slope,
                                                 &pitch_intercept);

  free(yaw_work);
  free(pitch_work);
  free(time_work);

  if (yaw_result != RET_SUCCESS || pitch_result != RET_SUCCESS)
  {
    return RET_FAIL;
  }

  int   next_time       = vpes->next_timestamp;
  float predicted_yaw   = yaw_slope * next_time + yaw_intercept;
  float predicted_pitch = pitch_slope * next_time + pitch_intercept;

  *yaw                  = wrap_angle_360(predicted_yaw);
  *pitch                = clamp_pitch(predicted_pitch);

  return RET_SUCCESS;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Kalman filter viewport predictor
 * State : x = [yaw, pitch, yaw_vel, pitch_vel]
 * Model : constant-velocity  F = I + dt*[[0,0,1,0],[0,0,0,1],[0,0,0,0],[0,0,0,0]]
 * Obs   : z = [yaw, pitch],  H = [I2 | 0]
 * ══════════════════════════════════════════════════════════════════════════ */

static void kf_mat4_mul(float C[4][4],
                        const float A[4][4], const float B[4][4])
{
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++) {
      C[i][j] = 0.0f;
      for (int k = 0; k < 4; k++) C[i][j] += A[i][k] * B[k][j];
    }
}

static int kf_mat2_inv(float inv[2][2], const float m[2][2])
{
  float det = m[0][0]*m[1][1] - m[0][1]*m[1][0];
  if (fabsf(det) < 1e-8f) return 0;
  float id     =  1.0f / det;
  inv[0][0]    =  m[1][1] * id;  inv[0][1] = -m[0][1] * id;
  inv[1][0]    = -m[1][0] * id;  inv[1][1] =  m[0][0] * id;
  return 1;
}

/* Measurement update: x, P updated in place.
 * Exploits H = [I2|0]: H*x = x[0:2], H*P*H' = P[0:2,0:2], P*H' = P[:,0:2] */
static void kf_update(kalman_vp_t *kf, float z0, float z1)
{
  /* innovation — wrap yaw difference */
  float y0 = z0 - kf->x[0];
  float y1 = z1 - kf->x[1];
  if (y0 >  180.0f) y0 -= 360.0f;
  if (y0 < -180.0f) y0 += 360.0f;

  /* S = P[0:2,0:2] + R */
  float S[2][2] = {
    { kf->P[0][0] + kf->R[0][0],  kf->P[0][1] + kf->R[0][1] },
    { kf->P[1][0] + kf->R[1][0],  kf->P[1][1] + kf->R[1][1] }
  };
  float Sinv[2][2];
  if (!kf_mat2_inv(Sinv, S)) return;

  /* K = P[:,0:2] * Sinv   (4×2) */
  float K[4][2];
  for (int i = 0; i < 4; i++) {
    K[i][0] = kf->P[i][0]*Sinv[0][0] + kf->P[i][1]*Sinv[1][0];
    K[i][1] = kf->P[i][0]*Sinv[0][1] + kf->P[i][1]*Sinv[1][1];
  }

  /* x = x + K*y */
  for (int i = 0; i < 4; i++)
    kf->x[i] += K[i][0]*y0 + K[i][1]*y1;

  /* P = (I - K*H) * P
   * (K*H)[i,j] = K[i,j] for j<2, else 0  (because H has non-zero only in cols 0,1) */
  float IKH[4][4];
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++) {
      float kh = (j < 2) ? K[i][j] : 0.0f;
      IKH[i][j] = (i == j ? 1.0f : 0.0f) - kh;
    }
  float P_new[4][4];
  kf_mat4_mul(P_new, IKH, kf->P);
  memcpy(kf->P, P_new, sizeof(P_new));
}

/* Prediction step with dt = 1: x = F*x,  P = F*P*F' + Q
 * Exploits F sparsity: F[0] = [1,0,1,0], F[1] = [0,1,0,1], F[2]=F[3]=I rows */
static void kf_predict(kalman_vp_t *kf)
{
  float x_pred[4] = {
    kf->x[0] + kf->x[2],
    kf->x[1] + kf->x[3],
    kf->x[2],
    kf->x[3]
  };

  /* FP[i][:]:  rows 0,1 sum adjacent velocity rows; rows 2,3 pass through */
  float FP[4][4];
  for (int j = 0; j < 4; j++) {
    FP[0][j] = kf->P[0][j] + kf->P[2][j];
    FP[1][j] = kf->P[1][j] + kf->P[3][j];
    FP[2][j] = kf->P[2][j];
    FP[3][j] = kf->P[3][j];
  }
  /* FP * F':  (FP*F')[i][0] = FP[i][0]+FP[i][2],  [i][1] = FP[i][1]+FP[i][3],
   *            [i][2] = FP[i][2],                   [i][3] = FP[i][3]           */
  float P_pred[4][4];
  for (int i = 0; i < 4; i++) {
    P_pred[i][0] = FP[i][0] + FP[i][2] + kf->Q[i][0];
    P_pred[i][1] = FP[i][1] + FP[i][3] + kf->Q[i][1];
    P_pred[i][2] = FP[i][2]             + kf->Q[i][2];
    P_pred[i][3] = FP[i][3]             + kf->Q[i][3];
  }

  memcpy(kf->x, x_pred, sizeof(x_pred));
  memcpy(kf->P, P_pred, sizeof(P_pred));
}

RET vpes_kalman(viewport_prediction_t *vpes, float *yaw_out, float *pitch_out)
{
  if (!vpes || !yaw_out || !pitch_out) return RET_FAIL;

  kalman_vp_t *kf = &vpes->kalman;

  if (!kf->initialized) {
    int n    = vpes->sample_count;
    int idx1 = (vpes->current_index - 1 + vpes->history_size) % vpes->history_size;
    int idx0 = (vpes->current_index - 2 + vpes->history_size) % vpes->history_size;

    float y1 = vpes->yaw_history[idx1],   p1 = vpes->pitch_history[idx1];
    float y0 = (n >= 2) ? vpes->yaw_history[idx0]   : y1;
    float p0 = (n >= 2) ? vpes->pitch_history[idx0] : p1;

    float dy = y1 - y0;
    if (dy >  180.0f) dy -= 360.0f;
    if (dy < -180.0f) dy += 360.0f;

    kf->x[0] = y1;  kf->x[1] = p1;
    kf->x[2] = dy;  kf->x[3] = p1 - p0;

    memset(kf->P, 0, sizeof(kf->P));
    kf->P[0][0] = kf->P[1][1] = 100.0f;   /* high initial position uncertainty */
    kf->P[2][2] = kf->P[3][3] = 500.0f;   /* high initial velocity uncertainty */

    memset(kf->Q, 0, sizeof(kf->Q));
    kf->Q[0][0] = kf->Q[1][1] = 0.5f;    /* position process noise (deg²) */
    kf->Q[2][2] = kf->Q[3][3] = 5.0f;    /* velocity process noise (deg²/step²) */

    kf->R[0][0] = kf->R[1][1] = 9.0f;    /* meas noise: ~3° std dev */
    kf->R[0][1] = kf->R[1][0] = 0.0f;

    kf->initialized = 1;
  }

  /* 1. Update with latest measurement in history */
  int last = (vpes->current_index - 1 + vpes->history_size) % vpes->history_size;
  kf_update(kf, vpes->yaw_history[last], vpes->pitch_history[last]);

  /* 2. Predict one step ahead */
  kf_predict(kf);

  *yaw_out   = wrap_angle_360(kf->x[0]);
  *pitch_out = clamp_pitch(kf->x[1]);
  return RET_SUCCESS;
}