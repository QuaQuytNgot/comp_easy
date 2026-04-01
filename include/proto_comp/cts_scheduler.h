/*
 * cts_scheduler.h  —  Coordinated Tile Scheduler (CTS)
 *
 * Implements the three scheduling algorithms described in the paper:
 *
 *   SCHEDULER_GREEDY   — original greedy priority-based allocation
 *                        (kept for comparison / fallback)
 *
 *   SCHEDULER_RA_MPC   — Risk-Aware Model Predictive Control
 *                        Optimises a H-step trajectory to minimise stalls
 *                        while accounting for buffer and network uncertainty.
 *                        Objective (minimise):
 *                          Σ_{k=t}^{t+H} [-E[QoE_k] + λ1·Δr_k + λ2·Risk(B_k,τ)]
 *
 *   SCHEDULER_SLR      — Stochastic Lagrangian Relaxation
 *                        Converts bandwidth, CPU, and QoE-risk constraints
 *                        into Lagrange multipliers (λ, μ, γ) adjusted at
 *                        runtime via a subgradient update.
 *                        Lagrangian (maximise):
 *                          L = Σ p_i·Q(r_i)
 *                              − λ·(Σr_i − B̂)
 *                              − μ·(Σ C_proc_i − τ_CPU)
 *                              − γ·Risk(B, τ)
 *
 * All three share the same cts_input_t / cts_output_t interface so they
 * are interchangeable at call sites.
 *
 * CTS runtime logic (Section V-D of paper):
 *   1. Sample /proc/self/stat  → Δutime, Δstime
 *   2. Compute C_proc per tile type
 *   3. Call chosen solver → per-tile bitrate r_i
 *   4. If p_i drops sharply → RESET_STREAM (reclaim μ / CPU resource)
 *      If ρ_sys > threshold → reduce batch size
 */

#ifndef CTS_SCHEDULER_H
#define CTS_SCHEDULER_H

#include "define.h"
#include "abr.h"
#include "resource_monitor.h"

/* ── constants ────────────────────────────────────────────────────────────── */

#define CTS_NUM_QUALITIES      5     /* must match VIDEO_BIT_RATE length     */
#define CTS_MPC_HORIZON        3     /* look-ahead steps H for RA-MPC        */
#define CTS_P_VP_THRESHOLD     0.15f /* min p_i to be treated as viewport    */

/* RA-MPC penalty coefficients (λ1, λ2 in paper objective) */
#define CTS_MPC_LAMBDA1        1.2f  /* penalty on quality-level change Δr  */
#define CTS_MPC_LAMBDA2        2.0f  /* penalty on buffer-risk              */

/* Buffer risk: tiles that push buffer below this are penalised */
#define CTS_MPC_BUF_RISK_LOW   0.5f  /* seconds — danger zone               */

/* SLR initial multipliers */
#define CTS_SLR_LAMBDA_INIT    1.0f  /* bandwidth multiplier λ              */
#define CTS_SLR_MU_INIT        0.5f  /* CPU-load multiplier μ               */
#define CTS_SLR_GAMMA_INIT     0.8f  /* QoE-risk multiplier γ               */

/* SLR subgradient step size */
#define CTS_SLR_STEP           0.05f

/* SLR CPU threshold τ_CPU (normalised to [0,1]) */
#define CTS_SLR_TAU_CPU        0.75f

/* SLR QoE risk definition: buffer below this (seconds) = risk event */
#define CTS_SLR_BUF_RISK_THR   0.5f

/* Protocol processing overhead factors (same scale as resource_monitor) */
#define CTS_PROTO_FACTOR_H1    0.8f
#define CTS_PROTO_FACTOR_H2    1.0f
#define CTS_PROTO_FACTOR_H3    1.3f  /* QUIC user-space stack               */

/* ── per-tile result ──────────────────────────────────────────────────────── */
typedef struct {
    int   tile_id;
    float probability;       /* p_i(t)                                       */
    int   chosen_version;    /* quality index ∈ [0, CTS_NUM_QUALITIES)       */
    float quality_bps;       /* VIDEO_BIT_RATE[chosen_version]               */
    float c_proc;            /* estimated CPU cost for this tile             */
    float load_cpu;          /* normalised CPU contribution ∈ [0,1]         */
    float net_utility;       /* contribution to objective J                  */
    int   early_terminated;  /* set to 1 after RESET_STREAM                 */
} cts_tile_t;

/* ── shared input ─────────────────────────────────────────────────────────── */
typedef struct {
    float        *p_map;         /* p_i(t) — tile_count floats               */
    int           tile_count;
    float         bandwidth;     /* B(t) bytes/s                             */
    float         buffer_level;  /* current playback buffer (seconds)        */
    HTTP_VERSION  protocol;
    float         rho_sys;       /* from get_system_status()                 */
    float         c_proc_global; /* from resource_monitor_get_cproc()        */
    int           last_quality;  /* last chosen quality (for smoothness)     */
} cts_input_t;

/* ── shared output ────────────────────────────────────────────────────────── */
typedef struct {
    cts_tile_t *tiles;           /* caller-allocated array[tile_count]       */
    int         tile_count;
    float       total_bw_used;   /* Σ r_i (bytes/s)                         */
    float       total_cpu_load;  /* Σ load_cpu_i                            */
    float       objective_value; /* J value (meaning depends on algorithm)  */
    float       vp_avg_quality;  /* avg quality of tiles with p_i >= threshold */
} cts_output_t;

/* ── algorithm selector ───────────────────────────────────────────────────── */
typedef enum {
    SCHEDULER_GREEDY = 1,
    SCHEDULER_RA_MPC,
    SCHEDULER_SLR
} CTS_ALGORITHM;

/* ── SLR state (persists across segments for multiplier updates) ────────────── */
typedef struct {
    float lambda;   /* bandwidth multiplier                                   */
    float mu;       /* CPU-load multiplier                                    */
    float gamma;    /* QoE-risk multiplier                                    */
} slr_state_t;

/* ── public API ───────────────────────────────────────────────────────────── */

/* Populate cts_input_t with default values */
void cts_input_init(cts_input_t *in);

/* Initialise SLR state with starting multiplier values */
void slr_state_init(slr_state_t *state);

/*
 * cts_schedule()
 *
 * Main entry: select algorithm, run optimiser, fill cts_output_t.
 *
 * @param algo   SCHEDULER_GREEDY | SCHEDULER_RA_MPC | SCHEDULER_SLR
 * @param in     Fully-populated input context.
 * @param out    Output (out->tiles must be pre-allocated to tile_count).
 * @param slr    SLR persistent state (ignored for GREEDY / RA_MPC; may be NULL).
 * @return       RET_SUCCESS or RET_FAIL.
 */
RET cts_schedule(CTS_ALGORITHM     algo,
                 const cts_input_t *in,
                 cts_output_t      *out,
                 slr_state_t       *slr);

/* Helper: compute per-tile C_proc estimate */
float cts_cproc(int version, HTTP_VERSION protocol, float c_proc_global);

/* Helper: normalised CPU load ∈ [0,1] for one tile */
float cts_load_cpu(int version, HTTP_VERSION protocol);

/* Debug print of schedule result */
void cts_print_schedule(const cts_output_t *out, CTS_ALGORITHM algo);

#endif /* CTS_SCHEDULER_H */