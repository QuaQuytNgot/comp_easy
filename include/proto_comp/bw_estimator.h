#include "define.h"
#include "request_handler.h"

typedef struct bw_estimator_t bw_estimator_t;

struct bw_estimator_t
{
    bw_t dls_es;

  RET (*post)(const bw_t *, size_t *, bw_t *);
  RET (*get)(bw_estimator_t *, bw_t *);
};

RET bw_estimator_init(bw_estimator_t *self, int type);

RET bw_estimator_destroy(bw_estimator_t *self);

RET bw_estimator_post_harmonic_mean(const bw_t *self,
                                    size_t      arr_size,
                                    bw_t       *dls_arr);
RET bw_estimator_get_harmonic_mean(bw_estimator_t *self,
                                   bw_t           *dls_es);