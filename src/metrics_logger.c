#include "proto_comp/metrics_logger.h"

int metrics_logger_init(metrics_logger_t *logger, const char *filepath) {
    if (!logger || !filepath) return -1;

    logger->file = fopen(filepath, "w");
    if (!logger->file) {
        fprintf(stderr, "[metrics_logger] Errors: Can not create file %s\n", filepath);
        return -1;
    }

    fprintf(logger->file, "Segment,Network_BW_Mbps,Avg_VP_Bitrate_Kbps,Buffer_Level_s,Rebuffer_s,Smoothness,ET_Total_Count,CPU_Pressure_Rho,Seg_QoE,Total_QoE\n");
    fflush(logger->file);

    return 0;
}

void metrics_logger_write(metrics_logger_t *logger, const metrics_log_entry_t *entry) {
    if (!logger || !logger->file || !entry) return;

    fprintf(logger->file, "%llu,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%.4f,%.3f,%.3f\n",
            entry->segment_id,
            entry->network_bw_mbps,
            entry->avg_vp_bitrate_kbps,
            entry->buffer_level_s,
            entry->rebuffer_s,
            entry->smoothness,
            entry->et_count,
            entry->cpu_rho,
            entry->seg_qoe,
            entry->total_qoe);
            
    fflush(logger->file);
}

void metrics_logger_close(metrics_logger_t *logger) {
    if (logger && logger->file) {
        fclose(logger->file);
        logger->file = NULL;
    }
}