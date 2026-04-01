/*
 * resource_monitor.h
 *
 * Reads /proc/self/stat to compute the system-pressure ratio used by both
 * the RA-MPC and SLR schedulers:
 *
 *   rho_sys = Δstime / (Δutime + Δstime)    ∈ [0, 1]
 *   C_proc  = Δutime + ALPHA_STIME * Δstime  (composite CPU cost)
 *
 * rho_sys close to 1.0 means the process is spending most of its CPU budget
 * in kernel space (UDP / TLS processing) — high protocol-layer pressure.
 */

#ifndef RESOURCE_MONITOR_H
#define RESOURCE_MONITOR_H

#include "define.h"

/* Weight α on stime in C_proc (α > 1 penalises less-predictable kernel time) */
#define RM_ALPHA_STIME  1.85f

/* Path to stat file — overridable at compile time for unit tests */
#ifndef PROC_SELF_STAT_PATH
#define PROC_SELF_STAT_PATH "/proc/self/stat"
#endif

/* Single /proc/self/stat snapshot (fields 14-15) */
typedef struct {
    unsigned long utime;
    unsigned long stime;
} proc_stat_snapshot_t;

typedef struct {
    proc_stat_snapshot_t prev;
    proc_stat_snapshot_t curr;
    unsigned long        delta_utime;
    unsigned long        delta_stime;
    float                rho_sys;   /* Δstime / (Δutime + Δstime)          */
    float                c_proc;    /* Δutime + α·Δstime                    */
    int                  is_initialized;
} resource_monitor_t;

/* Initialise and take baseline snapshot */
RET   resource_monitor_init(resource_monitor_t *rm);

/* Advance snapshot, recompute rho_sys and c_proc — call once per segment */
RET   resource_monitor_update(resource_monitor_t *rm);

/* Returns rho_sys ∈ [0,1]; 0.0f if not yet updated */
float get_system_status(resource_monitor_t *rm);

/* Returns C_proc (clock-tick units); ≥ 1.0f always (safe for division) */
float resource_monitor_get_cproc(resource_monitor_t *rm);

#endif /* RESOURCE_MONITOR_H */