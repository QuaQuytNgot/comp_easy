#ifndef METRICS_EVALUATOR_H
#define METRICS_EVALUATOR_H

#include "cts_scheduler.h"
#include <stdio.h>
#include <math.h>

/* ── Cấu trúc lưu trữ trạng thái và kết quả đánh giá ─────────────────────── */
typedef struct {
    /* Trạng thái lưu lại từ segment trước */
    int   prev_versions[64];     /* Lưu tối đa 64 tiles, grid 8x6 = 48 */
    int   chunk_count;

    /* Cộng dồn kết quả cho toàn bộ video */
    float sum_sqv;               /* Tổng độ lệch không gian (Mbps) */
    float sum_oscillation;       /* Tổng dao động thời gian (Mbps) */
    float sum_wasted_bps;        /* Tổng băng thông lãng phí (bps) */
    float sum_total_bps;         /* Tổng băng thông thực tế đã dùng (bps) */
    
    /* Lịch sử ghi log (dùng để xuất ra file CSV vẽ đồ thị Python) */
    float log_sqv[1000];
    float log_oscillation[1000];
    float log_wasted_ratio[1000];
} metric_tracker_t;

void metric_tracker_init(metric_tracker_t *tracker);

/* * Hàm tính toán và cập nhật Metric sau mỗi lần tải xong 1 chunk 
 * Gọi hàm này NGAY SAU KHI thuật toán (SLR/MPC/Greedy) trả về kết quả
 */
void update_metrics(const cts_input_t *in, const cts_output_t *out, metric_tracker_t *tracker);

/* * Hàm in báo cáo tổng kết sau khi chạy xong 1 video 
 */
void print_evaluation_report(const metric_tracker_t *tracker, const char* algo_name);

#endif // METRICS_EVALUATOR_H