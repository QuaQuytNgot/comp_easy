#include "proto_comp/bw_estimator.h"
#include "proto_comp/define.h"
#include <math.h>

#define MPC_BUFFER_TARGET_S 2.5f

RET bw_estimator_init(bw_estimator_t *self, int type)
{
  *self        = (bw_estimator_t){0};
  self->dls_es = BW_DEFAULT;

  if (type == BW_ESTIMATOR_MPC) {
    self->post = NULL; 
    self->post_mpc = bw_estimator_post_mpc;
    self->get  = bw_estimator_get_harmonic_mean;
  } else {
    self->post = bw_estimator_post_harmonic_mean;
    self->post_mpc = NULL;
    self->get  = bw_estimator_get_harmonic_mean;
  }
  return RET_SUCCESS;
}

RET bw_estimator_destroy(bw_estimator_t *self)
{
  *self        = (bw_estimator_t){0};
  self->dls_es = 0;
  return RET_SUCCESS;
}
        
RET bw_estimator_post_harmonic_mean(const bw_t *bw_history,
                                    size_t      arr_size,
                                    bw_t       *result)
{
    if (bw_history == NULL || arr_size == 0 || result == NULL) return RET_FAIL;
    
    double sum_reciprocals = 0.0;
    int valid_samples = 0;
    size_t start = (arr_size > 10) ? (arr_size - 10) : 0;
    
    for (size_t i = start; i < arr_size; i++) {
        if (bw_history[i] > 0) {  
            sum_reciprocals += 1.0 / (double)bw_history[i];
            valid_samples++;
        }
    }
    
    if (valid_samples == 0) {
        *result = BW_DEFAULT;
        return RET_FAIL;
    }
    
    *result = (bw_t)(valid_samples / sum_reciprocals);
    return RET_SUCCESS;
}

/* MPC incorporates quadratic buffer penalty to avoid bang-bang oscillation */
RET bw_estimator_post_mpc(const bw_t *bw_history,
                          size_t      arr_size,
                          float       buffer_level_s,
                          bw_t       *result)
{
    bw_t harmonic_base;
    if (bw_estimator_post_harmonic_mean(bw_history, arr_size, &harmonic_base) != RET_SUCCESS) {
        return RET_FAIL;
    }

    /* Quadratic scaling based on buffer health */
    float buffer_diff = buffer_level_s - MPC_BUFFER_TARGET_S;
    float scaling_factor = 1.0f + 0.1f * (buffer_diff * fabsf(buffer_diff)); 

    if (scaling_factor > 1.5f) scaling_factor = 1.5f;
    if (scaling_factor < 0.5f) scaling_factor = 0.5f;

    *result = (bw_t)((float)harmonic_base * scaling_factor);
    return RET_SUCCESS;
}

RET bw_estimator_get_harmonic_mean(bw_estimator_t *self,
                                   bw_t           *dls_es)
{
  if (self->dls_es == 0) return RET_FAIL;
  *dls_es = self->dls_es;
  return RET_SUCCESS;
}