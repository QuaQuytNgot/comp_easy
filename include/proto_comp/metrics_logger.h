#ifndef METRICS_LOGGER_H
#define METRICS_LOGGER_H

#include <stdio.h>

typedef struct {
    FILE *file;
} metrics_logger_t;

typedef struct {
    unsigned long long segment_id;
    float network_bw_mbps;       // Estimated network bandwidth during the segment (Mbps)
    float avg_vp_bitrate_kbps;   // Average bitrate of a Viewport tile
    float buffer_level_s;        // Remaining buffer level
    float rebuffer_s;            // Rebuffering (stalling) time
    float smoothness;            // Quality fluctuation (smoothness)
    int   et_count;              // Number of early terminated tiles (Early Termination)
    float cpu_rho;               // CPU pressure/load (rho_sys)
    float seg_qoe;               // QoE score of the current segment
    float total_qoe;             // Cumulative total QoE
    float pred_err_deg;          // Angular prediction error (degrees)
    float vp_recall;             // Recall = TP / (TP + FN)
    float vp_iou;                // IoU   = TP / (TP + FP + FN)
} metrics_log_entry_t;

/**
 * Initialize the logger, open the file, and write the header row.
 * @param logger Pointer to the log management struct
 * @param filepath Path to the CSV file (e.g., "slr_metrics.csv")
 * @return 0 on success, -1 on failure
 */
int metrics_logger_init(metrics_logger_t *logger, const char *filepath);

/**
 * Write the data of a single segment as a new row in the CSV file.
 * @param logger Pointer to the log management struct
 * @param entry Pointer to the struct containing the data
 */
void metrics_logger_write(metrics_logger_t *logger, const metrics_log_entry_t *entry);

/**
 * Safely close the log file.
 * @param logger Pointer to the log management struct
 */
void metrics_logger_close(metrics_logger_t *logger);

#endif /* METRICS_LOGGER_H */