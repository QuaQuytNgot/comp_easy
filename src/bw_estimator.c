#include "proto_comp/bw_estimator.h"
#include "proto_comp/define.h"
#include <math.h>

// double harmonic_mean(double arr[], int len)
// {
//     int sum = 0;
//     for (int i = 0; i < len; i++)
//     {
//         sum += 1/arr[i];
//     }
//     int ans =  len/sum;
//     return ans;
// }

RET bw_estimator_init(bw_estimator_t *self, int type)
{
  *self        = (bw_estimator_t){0};
  self->dls_es = BW_DEFAULT;

  switch (type)
  {
  case BW_ESTIMATOR_HARMONIC:
    self->post = bw_estimator_post_harmonic_mean;
    self->get  = bw_estimator_get_harmonic_mean;
    break;

  default:
    self->post = bw_estimator_post_harmonic_mean;
    self->get  = bw_estimator_get_harmonic_mean;
    break;
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
    if (bw_history == NULL || arr_size == 0 || result == NULL) {
        return RET_FAIL;
    }
    
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

RET bw_estimator_get_harmonic_mean(bw_estimator_t *self,
                                   bw_t           *dls_es)
{
  if (self->dls_es == 0)
  {
    return RET_FAIL;
  }

  *dls_es = self->dls_es;
  return RET_SUCCESS;
}
