#include "proto_comp/resource_monitor.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Parse the system-wide CPU Pressure Stall Information line:
 *
 *   some avg10=1.23 avg60=... avg300=... total=...
 */
static RET read_cpu_psi(float *avg10)
{
    if (!avg10) return RET_FAIL;

    FILE *f = fopen(PROC_PRESSURE_CPU_PATH, "r");
    if (!f) return RET_FAIL;

    char line[256];
    RET result = RET_FAIL;
    while (fgets(line, sizeof(line), f)) {
        float value = 0.0f;
        if (sscanf(line, "some avg10=%f", &value) == 1) {
            if (value < 0.0f) value = 0.0f;
            if (value > 100.0f) value = 100.0f;
            *avg10 = value;
            result = RET_SUCCESS;
            break;
        }
    }

    fclose(f);
    return result;
}

/* Parse utime (field 14) and stime (field 15) from /proc/self/stat.
 * The comm field (2) may contain spaces and is wrapped in parentheses, so
 * we skip past the last ')' before scanning the numeric fields. */
static RET read_proc_stat(proc_stat_snapshot_t *snap)
{
    if (!snap) return RET_FAIL;

    FILE *f = fopen(PROC_SELF_STAT_PATH, "r");
    if (!f) { perror("[resource_monitor] fopen"); return RET_FAIL; }

    char buf[512];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return RET_FAIL; }
    fclose(f);

    char *pos = strrchr(buf, ')');
    if (!pos) return RET_FAIL;
    pos++;  /* skip past ')' */

    /* Fields after ')': state(3) ppid(4) pgrp(5) session(6) tty_nr(7)
     * tpgid(8) flags(9) minflt(10) cminflt(11) majflt(12) cmajflt(13)
     * utime(14) stime(15) */
    unsigned long utime = 0, stime = 0;
    int n = sscanf(pos,
                   " %*c %*d %*d %*d %*d %*d %*u %*lu %*lu %*lu %*lu"
                   " %lu %lu",
                   &utime, &stime);
    if (n != 2) {
        fprintf(stderr, "[resource_monitor] sscanf matched %d/2\n", n);
        return RET_FAIL;
    }

    snap->utime = utime;
    snap->stime = stime;
    return RET_SUCCESS;
}

RET resource_monitor_init(resource_monitor_t *rm)
{
    if (!rm) return RET_FAIL;
    memset(rm, 0, sizeof(*rm));

    if (read_proc_stat(&rm->curr) != RET_SUCCESS) return RET_FAIL;
    rm->prev           = rm->curr;
    rm->rho_sys        = 0.0f;
    rm->c_proc         = 1.0f;
    rm->psi_cpu        = 0.0f;
    rm->psi_available  = (read_cpu_psi(&rm->psi_cpu) == RET_SUCCESS);
    rm->is_initialized = 1;
    return RET_SUCCESS;
}

RET resource_monitor_update(resource_monitor_t *rm)
{
    if (!rm || !rm->is_initialized) return RET_FAIL;

    rm->prev = rm->curr;
    if (read_proc_stat(&rm->curr) != RET_SUCCESS) return RET_FAIL;

    rm->delta_utime = (rm->curr.utime >= rm->prev.utime)
                          ? (rm->curr.utime - rm->prev.utime) : 0UL;
    rm->delta_stime = (rm->curr.stime >= rm->prev.stime)
                          ? (rm->curr.stime - rm->prev.stime) : 0UL;

    unsigned long total = rm->delta_utime + rm->delta_stime;
    rm->rho_sys = (total > 0) ? ((float)rm->delta_stime / (float)total) : 0.0f;
    rm->c_proc  = (float)rm->delta_utime + RM_ALPHA_STIME * (float)rm->delta_stime;
    if (rm->c_proc < 1.0f) rm->c_proc = 1.0f;  /* safe floor for division */

    /* PSI is sampled at the same segment boundary. A transient read failure
     * keeps the last valid value but closes the availability flag. */
    resource_monitor_update_psi(rm);

    return RET_SUCCESS;
}

RET resource_monitor_update_psi(resource_monitor_t *rm)
{
    if (!rm || !rm->is_initialized) return RET_FAIL;

    float psi = rm->psi_cpu;
    if (read_cpu_psi(&psi) != RET_SUCCESS) {
        rm->psi_available = 0;
        return RET_FAIL;
    }

    rm->psi_cpu = psi;
    rm->psi_available = 1;
    return RET_SUCCESS;
}

float get_system_status(resource_monitor_t *rm)
{
    return (rm && rm->is_initialized) ? rm->rho_sys : 0.0f;
}

float resource_monitor_get_cproc(resource_monitor_t *rm)
{
    return (rm && rm->is_initialized) ? rm->c_proc : 1.0f;
}

float resource_monitor_get_psi_cpu(const resource_monitor_t *rm)
{
    return (rm && rm->is_initialized) ? rm->psi_cpu : 0.0f;
}

int resource_monitor_has_psi(const resource_monitor_t *rm)
{
    return rm && rm->is_initialized && rm->psi_available;
}
