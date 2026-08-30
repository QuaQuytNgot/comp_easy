#include "proto_comp/cts_scheduler.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifndef SLR_REF_BITRATE
#define SLR_REF_BITRATE 94712.0 /* This is VIDEO_BIT_RATE[0]   */
#endif

/* ── internal helpers ───────────────────────────────────────────────────── */
static RET run_pano(const cts_input_t *in, cts_output_t *out);
static float proto_factor(HTTP_VERSION p)
{
    switch (p)
    {
    case STREAM_HTTP_1_1:
        return CTS_PROTO_FACTOR_H1;
    case STREAM_HTTP_2_0:
        return CTS_PROTO_FACTOR_H2;
    case STREAM_HTTP_3_0:
        return CTS_PROTO_FACTOR_H3;
    default:
        return CTS_PROTO_FACTOR_H2;
    }
}

float cts_cproc(int v, HTTP_VERSION p, float cg)
{
    if (v < 0 || v >= CTS_NUM_QUALITIES)
        return cg;
    return ((float)VIDEO_BIT_RATE[v] / (float)VIDEO_BIT_RATE[0]) * proto_factor(p) * cg;
}

float cts_load_cpu(int v, HTTP_VERSION p)
{
    if (v < 0 || v >= CTS_NUM_QUALITIES)
        return 0.0f;
    float max_load = (float)VIDEO_BIT_RATE[CTS_NUM_QUALITIES - 1] * CTS_PROTO_FACTOR_H3;
    float load = ((float)VIDEO_BIT_RATE[v] * proto_factor(p)) / max_load;
    return (load > 1.0f) ? 1.0f : load;
}

static float buf_risk(float buffer_level, float threshold)
{
    float r = threshold - buffer_level;
    return (r > 0.0f) ? r : 0.0f;
}

typedef struct
{
    int idx;
    float pri;
} ranked_t;
static int cmp_desc(const void *a, const void *b)
{
    float pa = ((const ranked_t *)a)->pri;
    float pb = ((const ranked_t *)b)->pri;
    return (pb > pa) ? 1 : (pb < pa) ? -1
                                     : 0;
}

static float calc_spatial_smoothness(int tile_id, int chosen_v, const int *prev_versions, int N)
{
    if (!prev_versions)
        return 0.0f;

    float s_vi = 0.0f;
    float q_norm_v = (float)VIDEO_BIT_RATE[chosen_v] / SLR_REF_BITRATE;

    int cols = 8;
    int rows = 6;

    if (N != cols * rows)
        return 0.0f;

    int r = tile_id / cols;
    int c = tile_id % cols;

    int neighbors[4];
    int num_neighbors = 0;

    neighbors[num_neighbors++] = r * cols + (c - 1 + cols) % cols;
    neighbors[num_neighbors++] = r * cols + (c + 1) % cols;

    if (r < rows - 1)
    {
        neighbors[num_neighbors++] = (r + 1) * cols + c;
    }

    for (int k = 0; k < num_neighbors; k++)
    {
        int neighbor_idx = neighbors[k];
        float q_norm_neighbor_dev = (float)VIDEO_BIT_RATE[prev_versions[neighbor_idx]] / SLR_REF_BITRATE;

        float diff = q_norm_v - q_norm_neighbor_dev;
        if (diff > 0.0f)
        {
            s_vi += diff;
        }
    }

    return s_vi;
}

/* ── shared init ────────────────────────────────────────────────────────── */

void cts_input_init(cts_input_t *in)
{
    if (!in)
        return;
    memset(in, 0, sizeof(*in));
    in->protocol = STREAM_HTTP_2_0;
    in->buffer_level = MAX_BUFFER_SIZE / 2.0f;
    in->last_quality = 0;
}

void slr_state_init(slr_state_t *s)
{
    if (!s)
        return;
    s->lambda = CTS_SLR_LAMBDA_INIT;
    s->mu = CTS_SLR_MU_INIT;
    s->gamma = CTS_SLR_GAMMA_INIT;
}

/* ─────────────────────────────────────────────────────────────────────────
 * GREEDY  (không thay đổi)
 * ───────────────────────────────────────────────────────────────────────── */
static RET run_greedy(const cts_input_t *in, cts_output_t *out)
{
    int N = in->tile_count;
    float bw_left = in->bandwidth;
    float cpu_left = 1.0f;

    float lambda_eff = 1.5f * (1.0f + in->rho_sys);

    for (int i = 0; i < N; i++)
    {
        cts_tile_t *t = &out->tiles[i];
        t->tile_id = i;
        t->probability = in->p_map[i];
        t->chosen_version = 0;
        t->quality_bps = (float)VIDEO_BIT_RATE[0];
        t->c_proc = cts_cproc(0, in->protocol, in->c_proc_global);
        t->load_cpu = cts_load_cpu(0, in->protocol);
        t->net_utility = 0.0f;
        t->early_terminated = 0;
    }

    for (int i = 0; i < N; i++)
    {
        if (in->p_map[i] < CTS_P_VP_THRESHOLD)
            continue;
        float bw_cost = (float)VIDEO_BIT_RATE[0];
        float cpu_cost = cts_load_cpu(0, in->protocol);
        if (bw_left >= bw_cost && cpu_left >= cpu_cost)
        {
            bw_left -= bw_cost;
            cpu_left -= cpu_cost;
        }
    }

    ranked_t *rank = (ranked_t *)malloc(N * sizeof(ranked_t));
    if (!rank)
        return RET_FAIL;
    for (int i = 0; i < N; i++)
    {
        float dq = (float)(VIDEO_BIT_RATE[CTS_NUM_QUALITIES - 1] - VIDEO_BIT_RATE[0]);
        float cp = (out->tiles[i].c_proc > 0.0f) ? out->tiles[i].c_proc : 1.0f;
        rank[i].idx = i;
        rank[i].pri = (in->p_map[i] * dq) / cp;
    }
    qsort(rank, N, sizeof(ranked_t), cmp_desc);

    for (int r = 0; r < N; r++)
    {
        int i = rank[r].idx;
        cts_tile_t *t = &out->tiles[i];
        int cur = t->chosen_version;

        for (int v = cur + 1; v < CTS_NUM_QUALITIES; v++)
        {
            float dbw = (float)(VIDEO_BIT_RATE[v] - VIDEO_BIT_RATE[cur]);
            float dcpu = cts_load_cpu(v, in->protocol) - cts_load_cpu(cur, in->protocol);
            if (dbw > bw_left || dcpu > cpu_left)
                break;

            float dq = (float)(VIDEO_BIT_RATE[v] - VIDEO_BIT_RATE[cur]);
            float dc = cts_cproc(v, in->protocol, in->c_proc_global) - cts_cproc(cur, in->protocol, in->c_proc_global);
            float margin = t->probability * dq - lambda_eff * (1.0f - t->probability) * dc;
            if (margin <= 0.0f)
                break;

            bw_left -= dbw;
            cpu_left -= dcpu;
            cur = v;
        }

        t->chosen_version = cur;
        t->quality_bps = (float)VIDEO_BIT_RATE[cur];
        t->c_proc = cts_cproc(cur, in->protocol, in->c_proc_global);
        t->load_cpu = cts_load_cpu(cur, in->protocol);
        t->net_utility = t->probability * t->quality_bps - lambda_eff * (1.0f - t->probability) * t->c_proc;
    }
    free(rank);
    return RET_SUCCESS;
}

static RET run_ra_mpc(const cts_input_t *in, cts_output_t *out)
{
    int N = in->tile_count;
    float bw_left = in->bandwidth;
    float cpu_left = 1.0f;

    for (int i = 0; i < N; i++)
    {
        cts_tile_t *t = &out->tiles[i];
        t->tile_id = i;
        t->probability = in->p_map[i];
        t->chosen_version = 0;
        t->quality_bps = (float)VIDEO_BIT_RATE[0];
        t->c_proc = cts_cproc(0, in->protocol, in->c_proc_global);
        t->load_cpu = cts_load_cpu(0, in->protocol);
        t->net_utility = 0.0f;
        t->early_terminated = 0;
    }

    {
        float base_bw = (float)VIDEO_BIT_RATE[0] * (float)N;
        float base_cpu = cts_load_cpu(0, in->protocol) * (float)N;
        bw_left -= base_bw;
        cpu_left -= base_cpu;
        if (bw_left < 0.0f)
            bw_left = 0.0f;
        if (cpu_left < 0.0f)
            cpu_left = 0.0f;
    }

    ranked_t *rank = (ranked_t *)malloc(N * sizeof(ranked_t));
    if (!rank)
        return RET_FAIL;
    for (int i = 0; i < N; i++)
    {
        rank[i].idx = i;
        rank[i].pri = in->p_map[i];
    }
    qsort(rank, N, sizeof(ranked_t), cmp_desc);

    int prev_quality = in->last_quality;

    for (int r = 0; r < N; r++)
    {
        int i = rank[r].idx;
        cts_tile_t *t = &out->tiles[i];
        float p = t->probability;

        float best_score = -1e9f;
        int best_v = 0;

        for (int v = 0; v < CTS_NUM_QUALITIES; v++)
        {
            float dbw = (float)(VIDEO_BIT_RATE[v] - VIDEO_BIT_RATE[0]);
            float dcpu = cts_load_cpu(v, in->protocol) - cts_load_cpu(0, in->protocol);

            if (v > 0 && (dbw > bw_left || dcpu > cpu_left))
                continue;

            float sim_buf = in->buffer_level;
            float score_sum = 0.0f;
            int prev_v = prev_quality;

            for (int k = 0; k < CTS_MPC_HORIZON; k++)
            {
                float eq = p * (float)VIDEO_BIT_RATE[v] / 1000.0f;
                float dr = fabsf((float)VIDEO_BIT_RATE[v] - (float)VIDEO_BIT_RATE[prev_v]) / 1000.0f;
                float avail_bw = in->bandwidth > 0.0f ? in->bandwidth : 1.0f;
                float dl_time = (float)VIDEO_BIT_RATE[v] / avail_bw;
                if (sim_buf < dl_time)
                    sim_buf = 0.0f;
                else
                    sim_buf -= dl_time;
                sim_buf += SEGMENT_DURATION;
                if (sim_buf > MAX_BUFFER_SIZE)
                    sim_buf = MAX_BUFFER_SIZE;

                float risk = buf_risk(sim_buf, CTS_MPC_BUF_RISK_LOW);
                score_sum += eq - CTS_MPC_LAMBDA1 * dr - CTS_MPC_LAMBDA2 * risk;
                prev_v = v;
            }

            if (score_sum > best_score)
            {
                best_score = score_sum;
                best_v = v;
            }
        }

        if (best_v > 0)
        {
            float dbw = (float)(VIDEO_BIT_RATE[best_v] - VIDEO_BIT_RATE[0]);
            float dcpu = cts_load_cpu(best_v, in->protocol) - cts_load_cpu(0, in->protocol);
            bw_left -= dbw;
            cpu_left -= dcpu;
            if (bw_left < 0.0f)
                bw_left = 0.0f;
            if (cpu_left < 0.0f)
                cpu_left = 0.0f;
        }

        t->chosen_version = best_v;
        t->quality_bps = (float)VIDEO_BIT_RATE[best_v];
        t->c_proc = cts_cproc(best_v, in->protocol, in->c_proc_global);
        t->load_cpu = cts_load_cpu(best_v, in->protocol);
        t->net_utility = best_score;
        prev_quality = best_v;
    }
    free(rank);
    return RET_SUCCESS;
}

static RET run_slr(const cts_input_t *in, cts_output_t *out, slr_state_t *slr)
{
    if (!slr)
        return RET_FAIL;

    int N = in->tile_count;
    float ref = SLR_REF_BITRATE; /* VIDEO_BIT_RATE[0] = 94712 bps */

    /* 1. Calculate Buffer Slack ONCE outside the loops.
     * We DO NOT clamp it to 0.0f here, so that healthy buffers
     * produce a negative slack, allowing gamma to decay. */
    float buffer_slack_for_update = (CTS_SLR_BUF_RISK_THR - in->buffer_level) / CTS_SLR_BUF_RISK_THR;

    float risk_norm = (buffer_slack_for_update > 0.0f) ? buffer_slack_for_update : 0.0f;
    /* ── Per-tile dual decomposition ────────────────────────────────────── */
    for (int i = 0; i < N; i++)
    {
        cts_tile_t *t = &out->tiles[i];
        t->tile_id = i;
        t->probability = in->p_map[i];
        t->early_terminated = 0;

        float p = in->p_map[i];
        float best_dl = -1e30f;
        int best_v = 0;

        for (int v = 0; v < CTS_NUM_QUALITIES; v++)
        {
            /* cost bandwidth - this is mapping from version to bitrates (normalize) */
            float cost_bw = (float)VIDEO_BIT_RATE[v] / ref;
            float utility = logf(cost_bw + 1.0f);

            /* CPU Norm */
            float cp_raw = cts_cproc(v, in->protocol, in->c_proc_global);
            float cp_max = cts_cproc(CTS_NUM_QUALITIES - 1, STREAM_HTTP_3_0, in->c_proc_global);
            float cp_norm = (cp_max > 0.0f) ? (cp_raw / cp_max) : cp_raw;

            // penalty for spatial space (do phat khong gian)
            float s_vi = 0.0f;
            if (in->prev_versions)
            {
                s_vi = calc_spatial_smoothness(i, v, in->prev_versions, N);
            }

            /* L = p*U(v) - λ*C_bw(v) - μ*C_cpu(v) - γ*C_bw(v) */
            float dl = (p * utility) - (slr->lambda * cost_bw) -
                       (slr->mu * cp_norm) - (slr->gamma * risk_norm) - (slr->beta_smooth * s_vi);

            if (dl > best_dl)
            {
                best_dl = dl;
                best_v = v;
            }
        }

        t->chosen_version = best_v;
        t->quality_bps = (float)VIDEO_BIT_RATE[best_v];
        t->c_proc = cts_cproc(best_v, in->protocol, in->c_proc_global);
        t->load_cpu = cts_load_cpu(best_v, in->protocol);
        t->net_utility = best_dl;
    }

    /* ── Aggregate primal sums ───────────────────────────────────────────── */
    float sum_r = 0.0f, sum_cpu = 0.0f, sum_pq = 0.0f, sum_s = 0.0f;
    for (int i = 0; i < N; i++)
    {
        sum_r += out->tiles[i].quality_bps;
        sum_cpu += out->tiles[i].load_cpu;

        float chosen_cost_bw = out->tiles[i].quality_bps / ref;
        sum_pq += in->p_map[i] * logf(chosen_cost_bw + 1.0f);

        if (in->prev_versions)
        {
            sum_s += calc_spatial_smoothness(i, out->tiles[i].chosen_version, in->prev_versions, N);
        }
    }

    /* Objective value logging (use clamped risk here so objective isn't artificially inflated) */
    float actual_risk = (buffer_slack_for_update > 0.0f) ? buffer_slack_for_update : 0.0f;

    out->objective_value = sum_pq - slr->lambda * (sum_r - in->bandwidth) / ref - slr->mu * (sum_cpu - CTS_SLR_TAU_CPU * (float)N) - slr->gamma * actual_risk - slr->beta_smooth * (sum_s - CTS_SLR_TAU_SMOOTH);

    printf("[SLR] λ=%.4f μ=%.4f γ=%.4f  Obj=%.2f  Σr=%.2f Mbps  Σcpu=%.3f\n",
           slr->lambda, slr->mu, slr->gamma,
           out->objective_value, sum_r * 8.0f / 1e6f, sum_cpu);

    /* ── Subgradient update ──────────────────────────────────────────────── */
    float eta = CTS_SLR_STEP;

    float norm_bw_slack = (sum_r - in->bandwidth) / ((float)N * ref);
    float norm_cpu_slack = (sum_cpu - CTS_SLR_TAU_CPU * (float)N) / (float)N;
    float norm_smooth_slack = sum_s - CTS_SLR_TAU_SMOOTH;

    /* Multiplier updates - passing the UNCLAMPED buffer_slack so gamma can decay */
    slr->lambda += eta * norm_bw_slack;
    slr->mu += eta * norm_cpu_slack;
    slr->gamma += eta * buffer_slack_for_update;

    if (in->is_fluctuating)
    {
        slr->beta_smooth += eta * norm_smooth_slack;
    }
    else
    {
        slr->beta_smooth = 0.0f;
    }

    /* Apply lower bounds (projection to positive orthant) */
    if (slr->lambda < 0.0f)
        slr->lambda = 0.0f;
    if (slr->mu < 0.0f)
        slr->mu = 0.0f;
    if (slr->gamma < 0.0f)
        slr->gamma = 0.0f;
    if (slr->beta_smooth < 0.0f)
        slr->beta_smooth = 0.0f;

    /* Apply upper bounds */
    if (slr->lambda > 1.2f)
        slr->lambda = 1.2f;
    if (slr->mu > 1.0f)
        slr->mu = 1.0f;
    if (slr->gamma > 2.0f)
        slr->gamma = 2.0f;
    if (slr->beta_smooth > 1.5f)
        slr->beta_smooth = 1.5f;

    return RET_SUCCESS;
}

// static RET run_slr(const cts_input_t *in, cts_output_t *out, slr_state_t *slr)
// {
//     if (!slr) return RET_FAIL;

//     int   N   = in->tile_count;
//     float ref = SLR_REF_BITRATE;   /* VIDEO_BIT_RATE[0] = 94712 bps */

//     /* ── Per-tile dual decomposition ────────────────────────────────────── */
//     for (int i = 0; i < N; i++) {
//         cts_tile_t *t  = &out->tiles[i];
//         t->tile_id     = i;
//         t->probability = in->p_map[i];
//         t->early_terminated = 0;

//         float p       = in->p_map[i];
//         float best_dl = -1e30f;
//         int   best_v  = 0;

//         for (int v = 0; v < CTS_NUM_QUALITIES; v++) {
//             /* 1. Chi phí Băng thông (Tuyến tính) */
//             float cost_bw = (float)VIDEO_BIT_RATE[v] / ref;

//             /* 2. Giá trị Hữu dụng (Logarit để tạo đường cong bão hoà)
//              * Dùng logf(cost_bw + 1.0f) để đảm bảo v=0 thì log=ln(2) không bị âm */
//             float utility = logf(cost_bw + 1.0f);

//             float cp_raw  = cts_cproc(v, in->protocol, in->c_proc_global);
//             float cp_max  = cts_cproc(CTS_NUM_QUALITIES - 1, STREAM_HTTP_3_0, in->c_proc_global);
//             float cp_norm = (cp_max > 0.0f) ? (cp_raw / cp_max) : cp_raw;
//             float norm_risk_slack = (in->buffer_level < CTS_SLR_BUF_RISK_THR)
//                       ? ((CTS_SLR_BUF_RISK_THR - in->buffer_level) / CTS_SLR_BUF_RISK_THR)
//                       : 0.0f;
//             /* 3. Lagrangian mới: Tách bạch Utility và Cost */
//             // L = p * U(v) - λ * C_bw(v) - μ * C_cpu(v)
//             float dl = (p * utility) - (slr->lambda * cost_bw) - (slr->mu * cp_norm) - (slr->gamma * cost_bw);

//             if (dl > best_dl) {
//                 best_dl = dl;
//                 best_v  = v;
//             }
//         }

//         t->chosen_version = best_v;
//         t->quality_bps    = (float)VIDEO_BIT_RATE[best_v];
//         t->c_proc         = cts_cproc(best_v, in->protocol, in->c_proc_global);
//         t->load_cpu       = cts_load_cpu(best_v, in->protocol);
//         t->net_utility    = best_dl;
//     }

//     /* ── Aggregate primal sums ───────────────────────────────────────────── */
//     // float sum_r   = 0.0f, sum_cpu = 0.0f, sum_pq = 0.0f;
//     // for (int i = 0; i < N; i++) {
//     //     sum_r   += out->tiles[i].quality_bps;   /* bps */
//     //     sum_cpu += out->tiles[i].load_cpu;       /* ∈ [0,1] per tile */
//     //     sum_pq  += in->p_map[i] * out->tiles[i].quality_bps;
//     // }
//     // float actual_risk = (CTS_SLR_BUF_RISK_THR - in->buffer_level) / CTS_SLR_BUF_RISK_THR;

//     float sum_r   = 0.0f, sum_cpu = 0.0f, sum_pq = 0.0f;
//     for (int i = 0; i < N; i++) {
//         sum_r   += out->tiles[i].quality_bps;   /* bps */
//         sum_cpu += out->tiles[i].load_cpu;       /* ∈ [0,1] per tile */

//         float chosen_cost_bw = out->tiles[i].quality_bps / ref;
//         sum_pq  += in->p_map[i] * logf(chosen_cost_bw + 1.0f);
//     }

//     // float actual_risk = buf_risk(in->buffer_level, CTS_SLR_BUF_RISK_THR);
//     // float norm_risk_slack = (CTS_SLR_BUF_RISK_THR - in->buffer_level) / CTS_SLR_BUF_RISK_THR;

//     float norm_risk_slack = (in->buffer_level < CTS_SLR_BUF_RISK_THR)
//                       ? ((CTS_SLR_BUF_RISK_THR - in->buffer_level) / CTS_SLR_BUF_RISK_THR)
//                       : 0.0f;
//     float actual_risk = norm_risk_slack;

//     out->objective_value = sum_pq
//                          - slr->lambda * (sum_r   - in->bandwidth)
//                          - slr->mu     * (sum_cpu - CTS_SLR_TAU_CPU * (float)N)
//                          - slr->gamma  * actual_risk;

//     printf("[SLR] λ=%.4f μ=%.4f  Obj=%.2f  Σr=%.2f Mbps  Σcpu=%.3f\n",
//            slr->lambda, slr->mu,
//            out->objective_value, sum_r * 8.0f / 1e6f, sum_cpu);

//     /* ── [FIX-1] Subgradient update — consistent normalisation ──────────
//      *
//      * Both slack terms must be dimensionless and O(1) for eta=0.05 to work.
//      *
//      * BW slack:  (sum_r − B̂) / (N × ref)
//      *   sum_r in bps, B̂ = in->bandwidth in bps → difference in bps
//      *   divide by N×ref to get per-tile normalised slack ∈ [-8, +8]
//      *
//      * CPU slack: (sum_cpu − N×τ_CPU) / N
//      *   sum_cpu = Σ load_cpu_i where each load_cpu ∈ [0,1]
//      *   N×τ_CPU is the total CPU budget → divide by N for per-tile slack ∈ [-1,1]
//      *
//      * [FIX-2] in->bandwidth is the TOTAL budget for ALL N tiles (bps).
//      *   Do NOT compare avg_r against total bandwidth.               */
//     float eta = CTS_SLR_STEP;

//     float norm_bw_slack  = (sum_r - in->bandwidth) / ((float)N * ref);
//     float norm_cpu_slack = (sum_cpu - CTS_SLR_TAU_CPU * (float)N) / (float)N;

//     slr->lambda += eta * norm_bw_slack;
//     slr->mu     += eta * norm_cpu_slack;
//     slr->gamma  += eta * actual_risk;

//     if (slr->lambda < 0.0f) slr->lambda = 0.0f;
//     if (slr->mu     < 0.0f) slr->mu     = 0.0f;
//     if (slr->gamma  < 0.0f) slr->gamma  = 0.0f;

//     /* [FIX-3] Lower caps now that cp_norm is ∈[0,1]:
//      *   lambda cap: just above 1.0 so it can suppress p_i=1.0 VP tiles
//      *               when truly over budget, but not run away.
//      *   mu cap:     1.0 is enough since cp_norm ≤ 1.0                 */
//     if (slr->lambda > 1.2f) slr->lambda = 1.2f;
//     if (slr->mu     > 1.0f) slr->mu     = 1.0f;
//     if (slr->gamma  > 2.0f) slr->gamma  = 2.0f;

//     return RET_SUCCESS;
// }

/* ── aggregate_output ──────────────────────────────────────────────────── */
static void aggregate_output(const cts_input_t *in, cts_output_t *out,
                             float saved_obj)
{
    float sum_bw = 0.0f, sum_cpu = 0.0f, sum_util = 0.0f;
    float vp_q = 0.0f;
    int vp_cnt = 0;

    for (int i = 0; i < out->tile_count; i++)
    {
        cts_tile_t *t = &out->tiles[i];
        if (t->chosen_version < 0)
            t->chosen_version = 0;

        t->quality_bps = (float)VIDEO_BIT_RATE[t->chosen_version];
        t->c_proc = cts_cproc(t->chosen_version, in->protocol,
                              in->c_proc_global);
        t->load_cpu = cts_load_cpu(t->chosen_version, in->protocol);

        sum_bw += t->quality_bps;
        sum_cpu += t->load_cpu;
        sum_util += t->net_utility;

        if (in->p_map[i] >= CTS_P_VP_THRESHOLD)
        {
            vp_q += t->quality_bps;
            vp_cnt++;
        }
    }

    out->total_bw_used = sum_bw;
    out->total_cpu_load = sum_cpu;
    out->vp_avg_quality = (vp_cnt > 0) ? (vp_q / (float)vp_cnt) : 0.0f;
    out->objective_value = (saved_obj != 0.0f) ? saved_obj : sum_util;
}

/* ── debug print ───────────────────────────────────────────────────────── */
void cts_print_schedule(const cts_output_t *out, CTS_ALGORITHM algo)
{
    if (!out || !out->tiles)
        return;
    const char *name = (algo == SCHEDULER_RA_MPC) ? "RA-MPC" : (algo == SCHEDULER_SLR) ? "SLR"
                                                                                       : "GREEDY";
    printf("\n[CTS/%s] J=%.2f  BW=%.2f Mbps  CPU=%.3f  VP_avg=%.0f bps\n",
           name, out->objective_value,
           out->total_bw_used / 1e6f,
           out->total_cpu_load, out->vp_avg_quality);
    printf("  %-6s %-5s %-4s %-10s %-8s\n",
           "Tile", "p_i", "Ver", "Qual(bps)", "Utility");
    printf("  %-6s %-5s %-4s %-10s %-8s\n",
           "------", "-----", "----", "----------", "--------");
    for (int i = 0; i < out->tile_count; i++)
    {
        const cts_tile_t *t = &out->tiles[i];
        printf("  %-6d %-5.3f %-4d %-10.0f %-8.2f%s\n",
               t->tile_id, t->probability, t->chosen_version,
               t->quality_bps, t->net_utility,
               t->early_terminated ? " [ET]" : "");
    }
    printf("\n");
}

/*
 * compute_v_util()
 *
 * Precompute per-quality utility values: v_m = log₂(2 · R_m / R_0).
 *
 * This matches the logarithmic utility used in the paper (§4.3, Table 1):
 *   R_0=2 Mbps → v_0=1.000,  R_1=4 Mbps → v_1=2.000,  …
 *
 * We clamp to 0 if the argument would be ≤ 0 (defensive; can't happen with
 * valid VIDEO_BIT_RATE values where R_m ≥ R_0 > 0).
 */
static void compute_v_util(float v_util[CTS_NUM_QUALITIES])
{
    float r0 = (float)VIDEO_BIT_RATE[0];

    for (int m = 0; m < CTS_NUM_QUALITIES; m++)
    {
        float ratio = 2.0f * (float)VIDEO_BIT_RATE[m] / r0;
        v_util[m] = (ratio > 0.0f) ? log2f(ratio) : 0.0f;
    }
}

/*
 * compute_s_norm()
 *
 * Precompute per-quality normalised tile sizes: s̃_m = R_m / R_0.
 * Because S_m = R_m · δ and S_0 = R_0 · δ, the ratio is independent of δ.
 */
static void compute_s_norm(float s_norm[CTS_NUM_QUALITIES])
{
    float r0 = (float)VIDEO_BIT_RATE[0];

    for (int m = 0; m < CTS_NUM_QUALITIES; m++)
    {
        s_norm[m] = (float)VIDEO_BIT_RATE[m] / r0;
    }
}

/*
 * derive_V()
 *
 * Compute the Lyapunov trade-off parameter V from first principles (Eq. 5c):
 *
 *   V_max = (Q_max / δ  −  D) / (v_M  +  γ·δ)
 *   V     = BOLA360_V_SCALE · V_max
 *
 * Guarantees the buffer bound of Theorem 4.1 with headroom BOLA360_V_SCALE.
 *
 * Returns 0.0 if the denominator is non-positive or tile_count ≥ Q_max/δ
 * (degenerate configuration; caller should set V manually in that case).
 */
static float derive_V(float v_M, float gamma, float delta, int tile_count)
{
    float q_max     = (float)MAX_BUFFER_SIZE;          /* seconds (playback time) */
    float q_over_d  = q_max / delta;                   /* Q_max / δ */
    
    /* FIX: Vì q_max đang là thời gian playback, một chunk tải xong 
     * chỉ làm buffer tăng thêm 1 khoảng delta. Do đó ta trừ đi 1.0f 
     * thay vì tile_count. */
    float numerator = q_over_d - 1.0f;                 

    float denom     = v_M + gamma * delta;             /* v_M + γδ */

    if (numerator <= 0.0f || denom <= 0.0f) {
        /* Configuration too tight; fall back to a minimal safe V. */
        return 0.1f;
    }

    float V_max = numerator / denom;
    return BOLA360_V_SCALE * V_max;
}

/*
 * pl_cap_quality()
 *
 * BOLA360-PL: return the maximum quality index that does not violate the
 * per-tile bitrate cap derived from the current buffer and bandwidth.
 *
 * S_lim_per_tile  = Q(t) · (BOLA360_PL_BW_FACTOR · bandwidth) / D
 * R_cap           = S_lim_per_tile / δ   (R_m is bitrate, S_m = R_m · δ)
 * → max m with R_m ≤ R_cap.
 *
 * If the cap falls below quality 0 (buffer near zero), return 0 so the lowest
 * quality is still downloaded rather than nothing.
 */
static int pl_cap_quality(float buffer_level, float bandwidth,
                          int tile_count, float delta)
{
    if (tile_count <= 0 || delta <= 0.0f)
        return 0;

    /* R_cap in the same units as VIDEO_BIT_RATE */
    float s_lim = buffer_level * (BOLA360_PL_BW_FACTOR * bandwidth) / (float)tile_count;
    float r_cap = s_lim / delta;

    /* Find the highest m whose bitrate does not exceed r_cap. */
    int cap = 0;
    for (int m = 0; m < CTS_NUM_QUALITIES; m++)
    {
        if ((float)VIDEO_BIT_RATE[m] <= r_cap)
        {
            cap = m;
        }
    }
    return cap;
}

/* ── public API ──────────────────────────────────────────────────────────── */

void bola360_state_init(bola360_state_t *state,
                        int tile_count,
                        float gamma,
                        int pl_enabled)
{
    if (!state)
        return;
    memset(state, 0, sizeof(*state));

    state->gamma = (gamma > 0.0f) ? gamma : BOLA360_GAMMA_DEFAULT;
    state->delta = (float)SEGMENT_DURATION;
    state->pl_enabled = pl_enabled;

    /* Precompute utility and size tables. */
    compute_v_util(state->v_util);
    compute_s_norm(state->s_norm);

    /* Derive V from buffer capacity, tile count, and quality/γ parameters.
     * The caller can override state->V afterwards if a specific value is needed
     * (e.g. to trade off playback delay vs. QoE per Remark 2 in the paper). */
    float v_M = state->v_util[CTS_NUM_QUALITIES - 1];
    state->V = (tile_count > 0)
                   ? derive_V(v_M, state->gamma, state->delta, tile_count)
                   : 0.1f;

    state->last_idle = 0;
    state->last_idle_threshold = 0.0f;
}

RET abr_bola360_run(const cts_input_t *in,
                    cts_output_t *out,
                    bola360_state_t *state)
{
    /* ── argument validation ────────────────────────────────────────────── */
    if (!in || !out || !out->tiles || !in->p_map || !state)
        return RET_FAIL;
    if (in->tile_count <= 0 || state->V <= 0.0f || state->delta <= 0.0f)
        return RET_FAIL;

    int N = in->tile_count;
    float Q = in->buffer_level; /* Q(t_k) in seconds                   */
    float V = state->V;
    float gamma = state->gamma;
    float delta = state->delta;

    out->tile_count = N;
    out->objective_value = 0.0f;

    /* ── precompute idle threshold (Theorem 4.1) ────────────────────────── *
     *
     *   idle_thresh = V · δ · (v_M + γ·δ)
     *
     * If Q exceeds this, BOLA360 enters the idle state: downloading any tile
     * at any quality would produce a negative score (buffer already saturated).
     */
    float v_M = state->v_util[CTS_NUM_QUALITIES - 1];
    float idle_thresh = V * delta * (v_M + gamma * delta);
    state->last_idle_threshold = idle_thresh;

    if (Q > idle_thresh)
    {
        /*
         * Idle state (§4.1): skip all tiles.
         * Set every tile to quality 0 and mark early_terminated so the caller
         * knows to withhold download requests and wait until Q drops.
         * The actual wait duration Δ is managed by the caller's event loop.
         */
        state->last_idle = 1;

        for (int i = 0; i < N; i++)
        {
            cts_tile_t *t = &out->tiles[i];
            t->tile_id = i;
            t->probability = in->p_map[i];
            t->chosen_version = 0;
            t->quality_bps = (float)VIDEO_BIT_RATE[0];
            t->c_proc = cts_cproc(0, in->protocol, in->c_proc_global);
            t->load_cpu = cts_load_cpu(0, in->protocol);
            t->net_utility = 0.0f;
            t->early_terminated = 1; /* signal: do NOT issue download        */
        }

        /* Aggregate fields — meaningful even in idle so the caller can report. */
        out->total_bw_used = 0.0f;
        out->total_cpu_load = 0.0f;
        out->vp_avg_quality = 0.0f;

        printf("[BOLA360] idle  Q=%.2fs > thresh=%.2fs  (V=%.3f γ=%.3f)\n",
               Q, idle_thresh, V, gamma);
        return RET_SUCCESS;
    }

    state->last_idle = 0;

    /* ── BOLA360-PL: compute per-tile bitrate cap ───────────────────────── *
     *
     *   S_lim = Q(t) · (0.5 · bandwidth) / D        per tile
     *   R_cap = S_lim / δ
     *   pl_cap_idx = max m with R_m ≤ R_cap
     *
     * This prevents download overrun during start-up / seek (§6).
     * Only active when state->pl_enabled = 1 and bandwidth is known.
     */
    int pl_max = CTS_NUM_QUALITIES - 1; /* default: uncapped                  */
    if (state->pl_enabled && in->bandwidth > 0.0f)
    {
        pl_max = pl_cap_quality(Q, in->bandwidth, N, delta);
    }

    /* ── per-tile BOLA360 optimisation ─────────────────────────────────── *
     *
     *   For each tile d independently (Theorem 4.3 — decomposable objective):
     *
     *     m*(d) = argmax_m  score(d, m)
     *
     *     score(d, m) = [ V · (v_m · p_d  +  γ·δ)  −  Q/δ ]  /  s̃_m
     *
     *   If the best score is ≤ 0, tile d is not downloaded (early_terminated).
     */
    float Q_over_delta = Q / delta; /* Q(t_k)/δ — reused per tile    */
    float sum_pq = 0.0f;            /* Σ p_d · v_{m*(d)} → U_K term   */
    float sum_bw = 0.0f;            /* Σ R_{m*(d)}                     */
    float sum_cpu = 0.0f;           /* Σ load_cpu_d                    */
    float vp_q = 0.0f;
    int vp_cnt = 0;

    for (int i = 0; i < N; i++)
    {
        cts_tile_t *t = &out->tiles[i];

        t->tile_id = i;
        t->probability = in->p_map[i];
        t->early_terminated = 0;

        float p = in->p_map[i];

        /* ── BOLA360 score computation (Eq. 5a) ────────────────────────── *
         *
         * For m = 0 .. CTS_NUM_QUALITIES − 1, bounded by BOLA360-PL cap:
         *
         *   numerator(m) = V · (v_m · p  +  γ·δ)  −  Q/δ
         *   score(m)     = numerator(m) / s̃_m
         *
         * Note: the score function is NOT necessarily monotone in m.
         * A higher bitrate increases the numerator (better utility) but also
         * increases the denominator (larger tile size), so the argmax trades
         * off quality gain against download cost.  This is the key insight
         * that makes BOLA360 buffer-adaptive: at low Q the numerator is
         * large (buffer not saturated), favouring high quality; at high Q the
         * numerator shrinks, naturally pushing selection down toward quality 0.
         */
        float best_score = -1e30f;
        int best_m = -1; /* -1 = no profitable quality found        */

        for (int m = 0; m <= pl_max; m++)
        {
            /* ── Guard: s̃_m must be positive ────────────────────────── */
            float s_m = state->s_norm[m];
            if (s_m <= 0.0f)
                continue;

            /* ── Core BOLA360 score ──────────────────────────────────── */
            float numerator = V * (state->v_util[m] * p + gamma * delta) - Q_over_delta;
            float score = numerator / s_m;

            if (score > best_score)
            {
                best_score = score;
                best_m = m;
            }
        }

        /* ── Idle / skip decision per tile ─────────────────────────────── *
         *
         * If the best score is ≤ 0, no quality level is profitable for tile d.
         * This can happen for very low-probability tiles when Q is moderate
         * (even quality 0 has a negative score).  The tile is "not downloaded."
         *
         * Practically: set to quality 0 and flag early_terminated = 1 so the
         * download scheduler skips issuing the QUIC stream for this tile.
         */
        if (best_m < 0 || best_score <= BOLA360_SCORE_EPSILON)
        {
            t->chosen_version = 0;
            t->quality_bps = (float)VIDEO_BIT_RATE[0];
            t->c_proc = cts_cproc(0, in->protocol, in->c_proc_global);
            t->load_cpu = cts_load_cpu(0, in->protocol);
            t->net_utility = 0.0f;
            t->early_terminated = 1;
            continue;
        }

        /* ── Accept the selected quality ────────────────────────────────── */
        t->chosen_version = best_m;
        t->quality_bps = (float)VIDEO_BIT_RATE[best_m];
        t->c_proc = cts_cproc(best_m, in->protocol, in->c_proc_global);
        t->load_cpu = cts_load_cpu(best_m, in->protocol);
        /*
         * net_utility stores the raw BOLA360 score for this tile.
         * It is used by cts_print_schedule() and can be compared across tiles
         * to understand how the algorithm allocated quality.
         */
        t->net_utility = best_score;

        /* Accumulate U_K: expected playback utility for this segment. */
        sum_pq += p * state->v_util[best_m];
        sum_bw += t->quality_bps;
        sum_cpu += t->load_cpu;

        if (p >= CTS_P_VP_THRESHOLD)
        {
            vp_q += t->quality_bps;
            vp_cnt++;
        }
    }

    /* ── Populate aggregate output fields ───────────────────────────────── *
     *
     * objective_value = Σ_d p_d · v_{m*(d)}
     *                 = expected playback utility for this segment
     *                 = time-average contribution to U_K (Eq. 1 in the paper)
     *
     * This is the quality side of the QoE objective (before the smoothness γ·R_K
     * term is added at the system level).  It lets the caller track whether
     * BOLA360 is converging toward the optimal U_K* value.
     */
    out->objective_value = sum_pq;
    out->total_bw_used = sum_bw;
    out->total_cpu_load = sum_cpu;
    out->vp_avg_quality = (vp_cnt > 0) ? (vp_q / (float)vp_cnt) : 0.0f;

    /* ── Diagnostic output ──────────────────────────────────────────────── */
    printf("[BOLA360] Q=%.2fs  idle_thresh=%.2fs  V=%.3f  γ=%.3f  "
           "U_K=%.4f  ΣR=%.2f Mbps  CPU=%.3f  VP_avg=%.0f bps\n",
           Q, idle_thresh, V, gamma,
           sum_pq,
           sum_bw * 8.0f / 1e6f,
           sum_cpu,
           out->vp_avg_quality);

    return RET_SUCCESS;
}

int abr_bola360_idle(const bola360_state_t *state)
{
    return state ? state->last_idle : 0;
}

//--------------------------------------------------------------------------------------------------------------------------
// PANO ALG ----------------------------------------------------------------------
static float pano_f_v(float speed_deg_s)
{
    /* Higher speed = less sensitive = higher JND multiplier */
    if (speed_deg_s <= 0.0f)
        return 1.0f;
    return 1.0f + 0.05f * speed_deg_s;
}

static float pano_f_l(float lum_change)
{
    /* Larger change = less sensitive */
    if (lum_change <= 0.0f)
        return 1.0f;
    return 1.0f + 0.002f * lum_change;
}

static float pano_f_d(float dof_diff)
{
    /* Greater difference in depth = less sensitive */
    if (dof_diff <= 0.0f)
        return 1.0f;
    return 1.0f + 0.3f * dof_diff;
}

/* Approximated PSPNR function incorporating 360JND (Equation 1 & 4) */
static float pano_estimate_pspnr(int quality_level, float p_i, float f_v, float f_l, float f_d)
{
    /* Base PSNR proxy based on bitrate scale */
    float base_psnr = 30.0f + (quality_level * 5.0f);

    /* Action-dependent ratio A(x1, x2, x3) */
    float a_ratio = f_v * f_l * f_d;

    /* 360JND effectively boosts the perceived PSNR score by filtering out imperceptible noise.
       We weight this by the viewport probability p_i to capture spatial attention. */
    return (base_psnr * a_ratio) * p_i;
}

/* DP Discretization parameters */
#define PANO_BW_BUCKETS 2000
#define PANO_MIN_BITRATE VIDEO_BIT_RATE[0]

static RET run_pano(const cts_input_t *in, cts_output_t *out)
{
    int N = in->tile_count;
    if (N <= 0)
        return RET_FAIL;

    /* Base cost calculation */
    float base_bw_total = (float)PANO_MIN_BITRATE * N;
    float bw_budget = in->bandwidth;

    /* Phase 1: Initialize to base quality 0 */
    for (int i = 0; i < N; i++)
    {
        cts_tile_t *t = &out->tiles[i];
        t->tile_id = i;
        t->probability = in->p_map[i];
        t->chosen_version = 0;
        t->quality_bps = (float)VIDEO_BIT_RATE[0];
        t->c_proc = cts_cproc(0, in->protocol, in->c_proc_global);
        t->load_cpu = cts_load_cpu(0, in->protocol);
        t->early_terminated = 0;
        t->net_utility = 0.0f;
    }

    if (bw_budget <= base_bw_total)
    {
        /* Insufficient bandwidth for upgrades; return base allocation */
        return RET_SUCCESS;
    }

    /* Phase 2: Pareto-pruning MCKP via Discrete DP */
    float available_bw = bw_budget - base_bw_total;
    float max_dbw = (float)(VIDEO_BIT_RATE[CTS_NUM_QUALITIES - 1] - VIDEO_BIT_RATE[0]) * N;

    /* Scale factor to map bandwidth to discrete buckets */
    float bw_scale = (float)PANO_BW_BUCKETS / (max_dbw > 0 ? max_dbw : 1.0f);
    int max_w = (int)(available_bw * bw_scale);
    if (max_w > PANO_BW_BUCKETS)
        max_w = PANO_BW_BUCKETS;

    float *dp = (float *)calloc(max_w + 1, sizeof(float));
    int **choices = (int **)malloc(N * sizeof(int *));
    if (!dp || !choices)
    {
        if (dp)
            free(dp);
        if (choices)
            free(choices);
        return RET_FAIL;
    }

    for (int i = 0; i < N; i++)
    {
        choices[i] = (int *)calloc(max_w + 1, sizeof(int));
    }

    float f_v = pano_f_v(in->viewpoint_speed_deg_s);

    /* DP Table Construction */
    for (int i = 0; i < N; i++)
    {
        float f_l = in->tile_lum_change ? pano_f_l(in->tile_lum_change[i]) : 1.0f;
        float f_d = in->tile_dof_diff ? pano_f_d(in->tile_dof_diff[i]) : 1.0f;
        float p_i = in->p_map[i];

        /* Traverse backwards to simulate 0/1 knapsack behavior per tile */
        for (int w = max_w; w >= 0; w--)
        {
            float best_pspnr = dp[w];
            int best_v = 0; // Relative to current choice (0 means no upgrade)

            for (int v = 1; v < CTS_NUM_QUALITIES; v++)
            {
                float dbw = (float)(VIDEO_BIT_RATE[v] - VIDEO_BIT_RATE[0]);
                int weight = (int)(dbw * bw_scale);

                if (w >= weight)
                {
                    float utility_gain = pano_estimate_pspnr(v, p_i, f_v, f_l, f_d) -
                                         pano_estimate_pspnr(0, p_i, f_v, f_l, f_d);

                    float candidate_pspnr = dp[w - weight] + utility_gain;

                    if (candidate_pspnr > best_pspnr)
                    {
                        best_pspnr = candidate_pspnr;
                        best_v = v;
                    }
                }
            }
            dp[w] = best_pspnr;
            choices[i][w] = best_v;
        }
    }

    /* Phase 3: Backtrack to find exact tile assignments */
    int current_w = max_w;
    for (int i = N - 1; i >= 0; i--)
    {
        int chosen_v = choices[i][current_w];
        out->tiles[i].chosen_version = chosen_v;
        out->tiles[i].quality_bps = (float)VIDEO_BIT_RATE[chosen_v];
        out->tiles[i].c_proc = cts_cproc(chosen_v, in->protocol, in->c_proc_global);
        out->tiles[i].load_cpu = cts_load_cpu(chosen_v, in->protocol);

        float f_l = in->tile_lum_change ? pano_f_l(in->tile_lum_change[i]) : 1.0f;
        float f_d = in->tile_dof_diff ? pano_f_d(in->tile_dof_diff[i]) : 1.0f;
        out->tiles[i].net_utility = pano_estimate_pspnr(chosen_v, in->p_map[i], f_v, f_l, f_d);

        if (chosen_v > 0)
        {
            float dbw = (float)(VIDEO_BIT_RATE[chosen_v] - VIDEO_BIT_RATE[0]);
            current_w -= (int)(dbw * bw_scale);
        }
    }

    /* Cleanup */
    free(dp);
    for (int i = 0; i < N; i++)
        free(choices[i]);
    free(choices);

    return RET_SUCCESS;
}

//-----------------------------------------------------------------------------------------------------------------

// SALIENTVR ALG (Nguyên bản theo bài báo MobiCom '22) ---------------------------
#define SVR_ALPHA 0.1f
#define SVR_BETA 0.5f
#define SVR_GAMMA 1.0f

typedef struct
{
    int orig_idx;
    float saliency;
} svr_rank_t;

static int cmp_svr_rank(const void *a, const void *b)
{
    float sa = ((svr_rank_t *)a)->saliency;
    float sb = ((svr_rank_t *)b)->saliency;
    if (sa < sb)
        return 1;
    if (sa > sb)
        return -1;
    return 0;
}

/* Tính toán hàm mục tiêu: Eq. 1: reward_k = Q_k - alpha*DC_k - beta*DT_k */
static float calc_svr_reward(int *v, const float *saliency, int last_q, int rows, int cols)
{
    float q_k = 0.0f, dc_k = 0.0f, dt_k = 0.0f;
    int N = rows * cols;

    for (int j = 0; j < N; j++)
    {
        float s_j = saliency[j];

        /* Quality reward */
        q_k += s_j * (float)v[j];

        /* Temporal difference: DC_k */
        float diff_t = (float)abs(v[j] - last_q);
        dc_k += s_j * s_j * diff_t;

        /* Spatial difference: DT_k */
        int y = j / cols;
        int x = j % cols;
        float local_dt = 0.0f;
        int nei_count = 0;

        int left = y * cols + (x - 1 + cols) % cols;
        local_dt += abs(v[j] - v[left]);
        nei_count++;

        int right = y * cols + (x + 1) % cols;
        local_dt += abs(v[j] - v[right]);
        nei_count++;

        if (y > 0)
        {
            int up = (y - 1) * cols + x;
            local_dt += abs(v[j] - v[up]);
            nei_count++;
        }
        if (y < rows - 1)
        {
            int down = (y + 1) * cols + x;
            local_dt += abs(v[j] - v[down]);
            nei_count++;
        }

        dt_k += (s_j * local_dt) / (float)nei_count;
    }

    return q_k - (SVR_ALPHA * dc_k) - (SVR_BETA * dt_k);
}

static RET run_salientvr(const cts_input_t *in, cts_output_t *out)
{
    int N = in->tile_count;
    if (N <= 0)
        return RET_FAIL;

    int cols = NO_OF_COLS;
    int rows = N / cols;
    if (rows * cols != N)
        cols = N;
    float seg_dur = SEGMENT_DURATION;

    /* 1. Xếp hạng Saliency để duy trì Ràng buộc Đơn điệu (Monotonicity) */
    svr_rank_t *rank = (svr_rank_t *)malloc(N * sizeof(svr_rank_t));
    for (int i = 0; i < N; i++)
    {
        rank[i].orig_idx = i;
        rank[i].saliency = in->p_map[i];
    }
    qsort(rank, N, sizeof(svr_rank_t), cmp_svr_rank);

    /* 2. Tính toán Ngân sách (Khắc phục Base Cost để không sập mạng) */
    float available_time = in->buffer_level - SVR_GAMMA;
    if (available_time < seg_dur)
    {
        available_time = seg_dur;
    }
    
    float max_size_bits = available_time * in->bandwidth;
    float base_bits_total = N * (float)VIDEO_BIT_RATE[0] * seg_dur;
    float upgrade_budget_bits = max_size_bits - base_bits_total;

    int *best_v = (int *)calloc(N, sizeof(int));
    int *cur_v = (int *)calloc(N, sizeof(int));
    int *mapped_v = (int *)calloc(N, sizeof(int));

    /* Gán cấu trúc tĩnh mặc định */
    for (int i = 0; i < N; i++)
    {
        out->tiles[i].tile_id = i;
        out->tiles[i].probability = in->p_map[i];
        out->tiles[i].c_proc = 0;
        out->tiles[i].load_cpu = 0;
    }

    /* Nếu không đủ mạng, trả về tất cả Version 0 */
    if (upgrade_budget_bits <= 0.0f)
    {
        for (int i = 0; i < N; i++)
        {
            out->tiles[i].chosen_version = 0;
            out->tiles[i].quality_bps = (float)VIDEO_BIT_RATE[0];
            out->tiles[i].net_utility = 0.0f;
        }
        free(rank);
        free(best_v);
        free(cur_v);
        free(mapped_v);
        return RET_SUCCESS;
    }

    /* 3. GREEDY INITIALIZATION (Tạo điểm khởi đầu tốt cho SA) */
    float current_upgrade_spent = 0.0f;
    for (int i = 0; i < N; i++)
    {
        while (cur_v[i] < CTS_NUM_QUALITIES - 1)
        {
            float cost_upgrade = (VIDEO_BIT_RATE[cur_v[i] + 1] - VIDEO_BIT_RATE[cur_v[i]]) * seg_dur;

            /* Monotonicity check */
            if (i > 0 && (cur_v[i] + 1) > cur_v[i - 1])
                break;

            if (current_upgrade_spent + cost_upgrade <= upgrade_budget_bits)
            {
                cur_v[i]++;
                current_upgrade_spent += cost_upgrade;
            }
            else
            {
                break;
            }
        }
    }

    for (int k = 0; k < N; k++)
        mapped_v[rank[k].orig_idx] = cur_v[k];
    float cur_reward = calc_svr_reward(mapped_v, in->p_map, in->last_quality, rows, cols);
    float best_reward = cur_reward;
    memcpy(best_v, cur_v, N * sizeof(int));

    /* 4. SIMULATED ANNEALING (Tối ưu hóa Smoothness theo Bài báo) */
    float T = 10.0f;
    float T_min = 0.01f;
    float alpha_T = 0.99f;

    while (T > T_min)
    {
        int next_v[48];
        memcpy(next_v, cur_v, N * sizeof(int));

        int idx = rand() % N;
        int dir = (rand() % 2 == 0) ? 1 : -1;

        if (dir == 1)
        {
            next_v[idx]++;
            if (next_v[idx] >= CTS_NUM_QUALITIES)
                next_v[idx] = CTS_NUM_QUALITIES - 1;
            /* Ràng buộc đơn điệu: Cascade left */
            for (int k = idx - 1; k >= 0; k--)
            {
                if (next_v[k] < next_v[k + 1])
                    next_v[k] = next_v[k + 1];
            }
        }
        else
        {
            next_v[idx]--;
            if (next_v[idx] < 0)
                next_v[idx] = 0;
            /* Ràng buộc đơn điệu: Cascade right */
            for (int k = idx + 1; k < N; k++)
            {
                if (next_v[k] > next_v[k - 1])
                    next_v[k] = next_v[k - 1];
            }
        }

        /* Kiểm tra ngân sách */
        float test_upgrade_spent = 0.0f;
        for (int k = 0; k < N; k++)
        {
            mapped_v[rank[k].orig_idx] = next_v[k];
            test_upgrade_spent += (VIDEO_BIT_RATE[next_v[k]] - VIDEO_BIT_RATE[0]) * seg_dur;
        }

        if (test_upgrade_spent <= upgrade_budget_bits)
        {
            float next_reward = calc_svr_reward(mapped_v, in->p_map, in->last_quality, rows, cols);
            float delta_reward = next_reward - cur_reward;

            /* Xác suất chấp nhận của SA */
            if (delta_reward > 0.0f || ((float)rand() / RAND_MAX) < expf(delta_reward / T))
            {
                memcpy(cur_v, next_v, N * sizeof(int));
                cur_reward = next_reward;
                if (cur_reward > best_reward)
                {
                    best_reward = cur_reward;
                    memcpy(best_v, cur_v, N * sizeof(int));
                }
            }
        }
        T *= alpha_T;
    }

    /* 5. Ánh xạ kết quả cuối cùng ra Out */
    for (int k = 0; k < N; k++)
    {
        int orig = rank[k].orig_idx;
        out->tiles[orig].chosen_version = best_v[k];
        out->tiles[orig].quality_bps = (float)VIDEO_BIT_RATE[best_v[k]];
        out->tiles[orig].net_utility = best_reward;
    }

    free(rank);
    free(best_v);
    free(cur_v);
    free(mapped_v);
    return RET_SUCCESS;
}

// =========================================================================
// MOSAIC ALG (SELF-CONTAINED STATE VER.)
// =========================================================================
#define MOSAIC_GAMMA_1 0.9f
#define MOSAIC_GAMMA_2 0.3f
#define MOSAIC_MU_1 3.0f
#define MOSAIC_MU_2 4.0f
#define MOSAIC_MU_3 1.0f
#define MOSAIC_MU_4 2.0f

// 1. Khai báo mảng tĩnh để Mosaic tự theo dõi lịch sử segment
static int *s_mosaic_prev_versions = NULL;
static int s_mosaic_prev_n = 0;

// 1. SỬA HÀM TÍNH QOE: Bỏ o_ij, tính điểm QoE toàn cục dựa trên p_j cho mọi tile
static float calc_mosaic_qoe(const cts_input_t *in, int *rates, const int *actual_prev)
{
    int N = in->tile_count;

    float b_i = 0.0f;
    float mean_qp = 0.0f;
    float total_dl_bps = 0.0f;

    float r_min = (float)VIDEO_BIT_RATE[0];
    if (r_min <= 0.0f)
        r_min = 1.0f; // Safety fallback

    for (int j = 0; j < N; j++)
    {
        float p_j = in->p_map[j];
        float q_r = 0.0f;

        // Tile luôn >= 0 vì đã xóa bỏ cơ chế -1 (Early Terminate)
        if (rates[j] >= 0)
        {
            q_r = (float)VIDEO_BIT_RATE[rates[j]] / r_min;
            total_dl_bps += (float)VIDEO_BIT_RATE[rates[j]];
        }

        // Tính trực tiếp kỳ vọng chất lượng toàn cục dựa trên xác suất p_j của tile
        b_i += p_j * q_r;
        mean_qp += q_r * p_j;
    }

    // s_i (penalty cho tile bị miss) = 0 vì ta tải toàn bộ 360 độ
    float q_i = MOSAIC_GAMMA_1 * b_i;
    mean_qp = (N > 0) ? (mean_qp / N) : 0.0f;

    float var = 0.0f;
    for (int j = 0; j < N; j++)
    {
        float q_r = (rates[j] >= 0) ? ((float)VIDEO_BIT_RATE[rates[j]] / r_min) : 0.0f;
        float diff = (q_r * in->p_map[j]) - mean_qp;
        var += diff * diff;
    }
    float e_i = sqrtf(var / N);

    float dl_time = (in->bandwidth > 0.0f) ? (total_dl_bps / in->bandwidth) : 0.0f;
    float t_i = dl_time - in->buffer_level;
    if (t_i < 0.0f)
        t_i = 0.0f;

    float g_i = 0.0f;

    if (actual_prev)
    {
        float prev_b_i = 0.0f;
        for (int j = 0; j < N; j++)
        {
            float prev_q_r = 0.0f;
            if (actual_prev[j] >= 0)
            {
                prev_q_r = (float)VIDEO_BIT_RATE[actual_prev[j]] / r_min;
            }
            prev_b_i += in->p_map[j] * prev_q_r;
        }
        float prev_q_i = MOSAIC_GAMMA_1 * prev_b_i;
        g_i = fabsf(q_i - prev_q_i);
    }

    return MOSAIC_MU_1 * q_i - MOSAIC_MU_2 * t_i - MOSAIC_MU_3 * g_i - MOSAIC_MU_4 * e_i;
}

// 2. SỬA HÀM RUN_MOSAIC: Khởi tạo tất cả tile bằng 0 và bỏ cơ chế Early Terminate
static RET run_mosaic(const cts_input_t *in, cts_output_t *out)
{
    int N = in->tile_count;
    if (N <= 0)
        return RET_FAIL;
    int cols = 8;

    // Khởi tạo mảng ghi nhớ nội bộ nếu chưa có
    if (s_mosaic_prev_versions == NULL || s_mosaic_prev_n != N)
    {
        if (s_mosaic_prev_versions)
            free(s_mosaic_prev_versions);
        s_mosaic_prev_versions = (int *)malloc(N * sizeof(int));
        // SỬA: Đặt base version là 0 thay vì -1
        for (int i = 0; i < N; i++)
            s_mosaic_prev_versions[i] = 0;
        s_mosaic_prev_n = N;
    }

    const int *actual_prev = in->prev_versions ? in->prev_versions : s_mosaic_prev_versions;

    int *rates = (int *)malloc(N * sizeof(int));
    int *best_rates = (int *)malloc(N * sizeof(int));
    int *r_tmp = (int *)malloc(N * sizeof(int));

    if (!rates || !best_rates || !r_tmp)
    {
        if (rates)
            free(rates);
        if (best_rates)
            free(best_rates);
        if (r_tmp)
            free(r_tmp);
        return RET_FAIL;
    }

    // 1. KHỞI TẠO TẤT CẢ TILE Ở BASE QUALITY (Version 0)
    float base_bw = 0.0f;
    for (int i = 0; i < N; i++)
    {
        rates[i] = 0;
        best_rates[i] = 0;
        base_bw += (float)VIDEO_BIT_RATE[0];
    }

    // 2. Điểm QoE nền tảng với mọi tile ở version 0
    float max_qoe = calc_mosaic_qoe(in, rates, actual_prev);
    int is_updated = 1;

    // Nếu chỉ tải Base Quality đã vượt băng thông, bỏ qua vòng lặp nâng cấp
    if (base_bw > in->bandwidth)
    {
        is_updated = 0;
    }

    // Vòng lặp Greedy Knapsack
    while (is_updated)
    {
        is_updated = 0;

        for (int i = 0; i < N; i++)
        {
            memcpy(r_tmp, rates, N * sizeof(int));

            int vp_r = i / cols;
            int vp_c = i % cols;
            int upgrade_attempted = 0;

            for (int j = 0; j < N; j++)
            {
                int r = j / cols;
                int c = j % cols;
                int c_diff = abs(vp_c - c);
                if (c_diff > cols / 2)
                    c_diff = cols - c_diff;

                // Thử nâng cấp 1 khối 3x3
                if (abs(vp_r - r) <= 1 && c_diff <= 1)
                {
                    if (r_tmp[j] < CTS_NUM_QUALITIES - 1)
                    {
                        r_tmp[j]++;
                        upgrade_attempted = 1;
                    }
                }
            }

            if (!upgrade_attempted)
                continue;

            // Check băng thông
            float total_dl_bps = 0.0f;
            for (int j = 0; j < N; j++)
            {
                total_dl_bps += (float)VIDEO_BIT_RATE[r_tmp[j]];
            }

            if (total_dl_bps > in->bandwidth)
                continue;

            // Gọi hàm tính điểm QoE đã được sửa để đánh giá TOÀN CỤC
            float current_qoe = calc_mosaic_qoe(in, r_tmp, actual_prev);

            if (current_qoe > max_qoe)
            {
                max_qoe = current_qoe;
                memcpy(best_rates, r_tmp, N * sizeof(int));
                is_updated = 1;
            }
        }

        if (is_updated)
        {
            memcpy(rates, best_rates, N * sizeof(int));
        }
    }

    // Trích xuất kết quả
    for (int i = 0; i < N; i++)
    {
        cts_tile_t *t = &out->tiles[i];
        t->tile_id = i;
        t->probability = in->p_map[i];

        t->chosen_version = rates[i];
        t->quality_bps = (float)VIDEO_BIT_RATE[rates[i]];
        t->c_proc = cts_cproc(rates[i], in->protocol, in->c_proc_global);
        t->load_cpu = cts_load_cpu(rates[i], in->protocol);

        // CHUẨN BÀI BÁO: KHÔNG SỬ DỤNG CƠ CHẾ BỎ QUA TILE
        t->early_terminated = 0;

        t->net_utility = max_qoe;

        // Lưu lại quyết định cho segment tiếp theo
        s_mosaic_prev_versions[i] = rates[i];
    }

    free(rates);
    free(best_rates);
    free(r_tmp);
    return RET_SUCCESS;
}

/* ── main dispatch ─────────────────────────────────────────────────────── */
RET cts_schedule(CTS_ALGORITHM algo,
                 const cts_input_t *in,
                 cts_output_t *out,
                 slr_state_t *slr)
{
    if (!in || !out || !out->tiles || !in->p_map || in->tile_count <= 0)
        return RET_FAIL;

    out->tile_count = in->tile_count;
    out->objective_value = 0.0f;

    RET ret;
    switch (algo)
    {
    case SCHEDULER_GREEDY:
        ret = run_greedy(in, out);
        break;
    case SCHEDULER_RA_MPC:
        ret = run_ra_mpc(in, out);
        break;
    case SCHEDULER_SLR:
        ret = run_slr(in, out, slr);
        break;
    case SCHEDULER_BOLA360:
        ret = abr_bola360_run(in, out, (bola360_state_t *)slr);
        break;
    case SCHEDULER_PANO:
        ret = run_pano(in, out);
        break;
    case SCHEDULER_MOSAIC:
        ret = run_mosaic(in, out);
        break;
    case SCHEDULER_SALIENTVR:
        ret = run_salientvr(in, out);
    default:
        return RET_FAIL;
    }
    if (ret != RET_SUCCESS)
        return RET_FAIL;

    float saved_obj = out->objective_value;
    aggregate_output(in, out, saved_obj);
    return RET_SUCCESS;
}