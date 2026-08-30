/*
 * resource_monitor.h
 *
 * Reads /proc/self/stat and /proc/pressure/cpu to expose both process-local
 * CPU accounting and system-wide CPU contention:
 *
 *   rho_sys = Δstime / (Δutime + Δstime)    ∈ [0, 1]
 *   C_proc  = Δutime + ALPHA_STIME * Δstime  (composite CPU cost)
 *   pi_cpu  = PSI "some" avg10                    ∈ [0, 100]
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

/* Linux Pressure Stall Information path; overridable for unit tests. */
#ifndef PROC_PRESSURE_CPU_PATH
#define PROC_PRESSURE_CPU_PATH "/proc/pressure/cpu"
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
    float                psi_cpu;   /* PSI "some" avg10 percentage, [0,100]  */
    int                  psi_available;
    int                  is_initialized;
} resource_monitor_t;

/* Initialise and take baseline snapshot */
RET   resource_monitor_init(resource_monitor_t *rm);

/* Advance snapshot, recompute rho_sys and c_proc — call once per segment */
RET   resource_monitor_update(resource_monitor_t *rm);

/* Refresh only PSI. Polling code uses this without disturbing stat deltas. */
RET   resource_monitor_update_psi(resource_monitor_t *rm);

/* Returns rho_sys ∈ [0,1]; 0.0f if not yet updated */
float get_system_status(resource_monitor_t *rm);

/* Returns C_proc (clock-tick units); ≥ 1.0f always (safe for division) */
float resource_monitor_get_cproc(resource_monitor_t *rm);

/* Returns Linux PSI cpu "some" avg10 in percent, or the last valid sample. */
float resource_monitor_get_psi_cpu(const resource_monitor_t *rm);

/* True when the most recent PSI read succeeded. */
int   resource_monitor_has_psi(const resource_monitor_t *rm);

#endif /* RESOURCE_MONITOR_H */
