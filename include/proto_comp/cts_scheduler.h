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
 * 
 * Public interface
 * ────────────────
 *   1.  bola360_state_t         – persistent state across segment decisions
 *   2.  bola360_state_init()    – zero + derive V from buffer / quality params
 *   3.  abr_bola360_run()       – main per-segment bitrate selector
 *   4.  abr_bola360_idle()      – true iff the last call entered the idle state
 * 
 * Algorithm overview (Eq. 5a, Algorithm 1)
 * ──────────────────────────────────────────
 *   At each segment boundary, for every tile d:
 *
 *     m*(d) = argmax_m  score(d, m)
 *
 *   where:
 *
 *     score(d, m) = [ V · (v_m · p_{k,d}  +  γ·δ)  −  Q(t_k)/δ ]  /  s̃_m
 *
 *     v_m   = log₂(2 · R_m / R_0)         quality utility (Table 1 values)
 *     s̃_m  = R_m / R_0                    normalised segment size (δ cancels)
 *     V     > 0                            Lyapunov trade-off parameter
 *     γ     > 0                            playback-smoothness weight
 *     δ     = SEGMENT_DURATION (seconds)
 *     Q     = in->buffer_level (seconds)
 *
 *   Buffer capacity guard (Theorem 4.1):
 *     idle_threshold = V · δ · (v_M  +  γ·δ)
 *     If Q > idle_threshold → idle state; no tile downloaded this round.
 *
 *   If score(d, m*(d)) ≤ 0 for all m, tile d is not downloaded (set to
 *   quality 0 and flagged with early_terminated = 1 so the caller can skip
 *   the download request or issue a RESET_STREAM).
 *
 * Optional extensions (controlled by flags in bola360_state_t)
 * ─────────────────────────────────────────────────────────────
 *   BOLA360-PL  (pl_enabled = 1):
 *     Limits each tile's bitrate to prevent download overrun during low-buffer
 *     or seek/start conditions.  Per-tile size cap (§6):
 *       S_lim = Q(t) · w_p(t) / 2D   [per tile in same units as R_m · δ]
 *     → cap_quality = largest m with  R_m ≤ S_lim / δ
 *
 *   Two-stage CPU gate (always active when rho_sys is provided):
 *     Stage 1:  p_i ≥ BOLA360_P_STAGE1_THRESHOLD  → always schedule
 *     Stage 2:  p_i <  BOLA360_P_STAGE1_THRESHOLD  → schedule only if
 *               in->rho_sys < BOLA360_RHO_STAGE2_MAX
 */
 

#ifndef CTS_SCHEDULER_H
#define CTS_SCHEDULER_H

#include "define.h"
#include "abr.h"
#include "resource_monitor.h"
#include <math.h>

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
#define CTS_SLR_LAMBDA_INIT    0.05f  /* bandwidth multiplier λ              */
#define CTS_SLR_MU_INIT        0.1f  /* CPU-load multiplier μ               */
#define CTS_SLR_GAMMA_INIT     0.2f  /* QoE-risk multiplier γ               */
#define CTS_SLR_BETA_INIT      0.0f  /* Smoothness multiplier beta */

/* SLR subgradient step size */
#define CTS_SLR_STEP           0.05f

/* SLR CPU threshold τ_CPU (normalised to [0,1]) */
#define CTS_SLR_TAU_CPU        0.75f

/* SLR Smoothness threshold*/
#define CTS_SLR_TAU_SMOOTH     0.5f

/* SLR QoE risk definition: buffer below this (seconds) = risk event */
#define CTS_SLR_BUF_RISK_THR   2.5f

/* Protocol processing overhead factors (same scale as resource_monitor) */
#define CTS_PROTO_FACTOR_H1    0.8f
#define CTS_PROTO_FACTOR_H2    1.0f
#define CTS_PROTO_FACTOR_H3    1.3f  /* QUIC user-space stack               */

/*
 * V_SCALE: fraction of the theoretical maximum V used by default.
 * V_max = (Q_max/δ − D) / (v_M + γδ).  We use 0.9 × V_max to stay
 * comfortably below the buffer bound of Theorem 4.1.
 */
#define BOLA360_V_SCALE            0.9f

/* Default γ: relative weight of the smoothness/rebuffer term.
 * Setting γ = 1/D equalises one tile's utility vs. one tile's smoothness
 * contribution (as noted in §4.3 of the paper).  Override via state.gamma. */
#define BOLA360_GAMMA_DEFAULT      0.1f

/* Minimum positive score to treat a quality level as "downloadable".
 * Guards against floating-point near-zero scores at the threshold boundary. */
#define BOLA360_SCORE_EPSILON      1e-6f

/* BOLA360-PL safety factor: effective bandwidth used for cap = bw * PL_FACTOR.
 * The paper uses 50% (0.5) of predicted bandwidth for the S_lim formula. */
#define BOLA360_PL_BW_FACTOR       0.5f

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

    int           *prev_versions; /*quality arry index of the previous segment*/  
    int           is_fluctuating; /*does user fluctuate their head (lien tuc)*/

    float         viewpoint_speed_deg_s;
    float         *tile_lum_change; /* Per tile luminance change [0, 255] */
    float         *tile_dof_diff;   /* Per tile DoF diff (dioptres) */
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
    SCHEDULER_SLR,
    SCHEDULER_BOLA360,
    SCHEDULER_PANO,
    SCHEDULER_SALIENTVR,
    SCHEDULER_MOSAIC
} CTS_ALGORITHM;

/* ── SLR state (persists across segments for multiplier updates) ────────────── */
typedef struct {
    float lambda;   /* bandwidth multiplier                                   */
    float mu;       /* CPU-load multiplier                                    */
    float gamma;    /* QoE-risk multiplier                                    */
    float beta_smooth;     /* Spatial smoothness multiplier            */
} slr_state_t;

typedef struct {
    /* ── primary Lyapunov parameters (set by bola360_state_init) ── */
    float  V;             /* trade-off parameter; larger V → higher quality,
                             longer playback delay (Remark 2 in paper)         */
    float  gamma;         /* smoothness weight γ > 0                           */
    float  delta;         /* segment duration δ (seconds); copied from
                             SEGMENT_DURATION for locality                     */

    /* ── precomputed quality-level tables (filled by bola360_state_init) ─── */
    float  v_util[CTS_NUM_QUALITIES]; /* v_m = log2(2·R_m/R_0), paper Table 1 */
    float  s_norm[CTS_NUM_QUALITIES]; /* s̃_m = R_m/R_0, normalised tile size  */

    /* ── optional extension flags ──────────────────────────────────────── */
    int    pl_enabled;    /* 1 = BOLA360-PL: cap bitrate to prevent overrun    */

    /* ── diagnostic / debug fields (read-only after a run call) ──────── */
    int    last_idle;     /* 1 if the last abr_bola360_run() call was idle     */
    float  last_idle_threshold; /* V·δ·(v_M + γ·δ) computed in last call      */
} bola360_state_t;

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

/*
 * bola360_state_init()
 *
 * Initialise a bola360_state_t.  Must be called once before the first
 * abr_bola360_run() call.
 *
 * The function:
 *   1. Precomputes v_util[] and s_norm[] from VIDEO_BIT_RATE[].
 *   2. Derives V automatically:
 *        V = V_SCALE · (Q_max/δ − D) / (v_M + γ·δ)
 *      where Q_max = MAX_BUFFER_SIZE, D = tile_count.
 *      If tile_count ≤ 0 the derivation is skipped; caller must set V manually.
 *   3. Stores gamma and delta (γ and δ).
 *
 * @param state       Pointer to uninitialised bola360_state_t.
 * @param tile_count  Number of tiles per chunk (D in the paper).
 * @param gamma       Smoothness weight γ.  Pass ≤ 0 to use BOLA360_GAMMA_DEFAULT.
 * @param pl_enabled  Non-zero to enable the BOLA360-PL bandwidth cap.
 */
void bola360_state_init(bola360_state_t *state,
                        int              tile_count,
                        float            gamma,
                        int              pl_enabled);

/*
 * abr_bola360_run()
 *
 * Execute one segment's worth of BOLA360 bitrate selection.
 * Fills out->tiles[0..tile_count-1].chosen_version (and associated fields).
 *
 * @param in    Fully-populated cts_input_t (buffer_level, p_map, bandwidth, …).
 * @param out   Pre-allocated cts_output_t (out->tiles must hold in->tile_count).
 * @param state Persistent BOLA360 state (updated each call).
 * @return      RET_SUCCESS, or RET_FAIL on invalid arguments.
 *
 * Tile fields populated per tile:
 *   chosen_version    – quality index m* ∈ [0, CTS_NUM_QUALITIES)
 *   quality_bps       – VIDEO_BIT_RATE[chosen_version]
 *   probability       – copied from in->p_map[i]
 *   c_proc            – CPU processing estimate (via cts_cproc)
 *   load_cpu          – normalised CPU load   (via cts_load_cpu)
 *   net_utility       – raw BOLA360 score for this tile (Eq. 5a numerator/s̃_m)
 *   early_terminated  – 1 if tile skipped (idle state or Stage-2 CPU gate)
 *
 * out->objective_value is set to Σ_d p_d · v_{m*(d)}, the expected playback
 * utility for this segment (corresponds to U_K term in the paper).
 */
RET abr_bola360_run(const cts_input_t *in,
                    cts_output_t      *out,
                    bola360_state_t   *state);

/*
 * abr_bola360_idle()
 *
 * Returns 1 if the most recent abr_bola360_run() call was in the idle state
 * (Q(t_k) exceeded the Theorem 4.1 buffer threshold), 0 otherwise.
 * The caller can use this to implement the "wait for Δ seconds" logic
 * described in §4.1 of the paper before re-attempting the decision.
 */
int abr_bola360_idle(const bola360_state_t *state);

#endif /* CTS_SCHEDULER_H */