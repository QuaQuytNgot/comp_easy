/* Real Example: 360 Video Streaming Pipeline
 * Pipeline: Init -> Loop(Viewport Prediction -> BW Estimation ->
 * ABR Selection -> Request Handler -> Update) -> Cleanup
 */

#include <proto_comp/buffer.h>
#include <proto_comp/define.h>
#include <proto_comp/request_handler.h>
#include <proto_comp/bw_estimator.h>
#include <proto_comp/abr.h>
#include <proto_comp/viewport_prediction.h>
#include <proto_comp/tile_selection.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Configuration
#define TOTAL_SEGMENTS 10
#define INITIAL_BUFFER_SIZE 0.0f
#define HISTORY_SIZE 100
#define PREDICTION_WINDOW 3  // P = 3
#define SERVER_ADDRESS "https://192.168.101.17:8443"

// Viewport dataset structure (you'll populate this with real data)
typedef struct {
    float yaw;
    float pitch;
    int timestamp_ms;
} ViewportSample;

int bitrate_fixed[] = {4, 3, 3, 2, 3, 3, 4, 4, 3, 4, 4, 3, 4, 4};

// Mock viewport dataset - Replace with your real data
ViewportSample viewport_dataset[] = {
    {0.0f, 0.0f, 0},
    {5.0f, 2.0f, 1000},
    {10.0f, 3.0f, 2000},
    {15.0f, 5.0f, 3000},
    {20.0f, 7.0f, 4000},
    {5.0f, 2.0f, 1000},
    {10.0f, 3.0f, 2000},
    {15.0f, 5.0f, 3000},
    {20.0f, 7.0f, 4000},
    {5.0f, 2.0f, 1000},
    {10.0f, 3.0f, 2000},
    {15.0f, 5.0f, 3000},
    {20.0f, 7.0f, 4000},
    {5.0f, 2.0f, 1000},
    {10.0f, 3.0f, 2000},
    {15.0f, 5.0f, 3000},
    {20.0f, 7.0f, 4000},
    {5.0f, 2.0f, 1000},
    {10.0f, 3.0f, 2000},
    {15.0f, 5.0f, 3000},
    {20.0f, 7.0f, 4000},
    {5.0f, 2.0f, 1000},
    {10.0f, 3.0f, 2000},
    {15.0f, 5.0f, 3000},
    {20.0f, 7.0f, 4000},
    {5.0f, 2.0f, 1000},
    {10.0f, 3.0f, 2000},
    {15.0f, 5.0f, 3000},
    {20.0f, 7.0f, 4000},
    {5.0f, 2.0f, 1000},
    {10.0f, 3.0f, 2000},
    {15.0f, 5.0f, 3000},
    {20.0f, 7.0f, 4000},{5.0f, 2.0f, 1000},
    {10.0f, 3.0f, 2000},
    {15.0f, 5.0f, 3000},
    {20.0f, 7.0f, 4000}
    // Add more samples...
};
#define DATASET_SIZE (sizeof(viewport_dataset) / sizeof(ViewportSample))

// Global metrics structure
typedef struct {
    COUNT segment_id;
    float predicted_yaw;
    float predicted_pitch;
    int num_viewport_tiles;
    bw_t predicted_bandwidth;
    int chosen_quality;
    float buffer_level;
    double total_download_time_ms;
    double avg_download_speed_mbps;
    size_t total_bytes_downloaded;
} SegmentMetrics;

// Function to print a header for the pipeline output
void print_pipeline_header() {
    printf("\n");
    printf("========================================\n");
    printf("   360 VIDEO STREAMING PIPELINE\n");
    printf("========================================\n");
    printf("Server: %s\n", SERVER_ADDRESS);
    printf("Total Segments: %d\n", TOTAL_SEGMENTS);
    printf("Prediction Window (P): %d\n", PREDICTION_WINDOW);
    printf("Initial Buffer: %.2f seconds\n", INITIAL_BUFFER_SIZE);
    printf("========================================\n\n");
}

// Function to print metrics for a single segment
void print_segment_metrics(SegmentMetrics *metrics) {
    printf("\n┌────────────────────────────────────────────────┐\n");
    printf("│ SEGMENT %llu METRICS                           \n", metrics->segment_id);
    printf("├────────────────────────────────────────────────┤\n");
    printf("│ Viewport Prediction:                           \n");
    printf("│   - Yaw: %.2f°, Pitch: %.2f°                  \n",
           metrics->predicted_yaw, metrics->predicted_pitch);
    printf("│   - Viewport Tiles: %d                         \n",
           metrics->num_viewport_tiles);
    printf("├────────────────────────────────────────────────┤\n");
    printf("│ Bandwidth Estimation:                          \n");
    printf("│   - Predicted BW: %.2f Mbps                    \n",
           metrics->predicted_bandwidth / 1000000.0);
    printf("├────────────────────────────────────────────────┤\n");
    printf("│ ABR Decision:                                  \n");
    printf("│   - Chosen Quality: %d (QP: %d)                \n",
           metrics->chosen_quality, tile_version_to_num(metrics->chosen_quality));
    printf("│   - Buffer Level: %.2f seconds                 \n",
           metrics->buffer_level);
    printf("├────────────────────────────────────────────────┤\n");
    printf("│ Download Performance:                          \n");
    printf("│   - Total Time: %.2f ms                        \n",
           metrics->total_download_time_ms);
    printf("│   - Avg Speed: %.2f Mbps                       \n",
           metrics->avg_download_speed_mbps);
    printf("│   - Total Data: %.2f MB                        \n",
           metrics->total_bytes_downloaded / 1048576.0);
    printf("└────────────────────────────────────────────────┘\n");
}

// Function to print a summary of the entire streaming session
void print_summary(SegmentMetrics *all_metrics, int count) {
    printf("\n");
    printf("╔════════════════════════════════════════════════╗\n");
    printf("║          STREAMING SESSION SUMMARY             ║\n");
    printf("╠════════════════════════════════════════════════╣\n");

    double total_time = 0, total_data = 0, avg_bw = 0;
    int quality_sum = 0;

    // Aggregate metrics from all segments
    for (int i = 0; i < count; i++) {
        total_time += all_metrics[i].total_download_time_ms;
        total_data += all_metrics[i].total_bytes_downloaded;
        avg_bw += all_metrics[i].avg_download_speed_mbps;
        quality_sum += all_metrics[i].chosen_quality;
    }

    // Print summary statistics
    printf("║ Total Segments Streamed: %d                    \n", count);
    printf("║ Total Download Time: %.2f seconds              \n", total_time / 1000.0);
    printf("║ Total Data Downloaded: %.2f MB                 \n", total_data / 1048576.0);
    printf("║ Average Bandwidth: %.2f Mbps                   \n", avg_bw / count);
    printf("║ Average Quality Level: %.2f                    \n", (float)quality_sum / count);
    printf("╚════════════════════════════════════════════════╝\n\n");
}

int main() {
    RET ret; // Return status variable

    print_pipeline_header(); // Print initial header information

    // ============================================
    // INITIALIZATION PHASE
    // ============================================
    printf("[INIT] Initializing components...\n");

    // 1. Initialize Request Handler
    request_handler_t request_handler;
    ret = request_handler_init(&request_handler,
                               SERVER_ADDRESS,
                               TOTAL_SEGMENTS,
                               5,  // version_count (Corresponds to QP levels: 38, 32, 28, 24, 20)
                               NO_OF_ROWS * NO_OF_COLS); // tile_count based on grid size
    if (ret != RET_SUCCESS) {
        printf("[ERROR] Failed to initialize request handler\n");
        return 1; // Exit if initialization fails
    }
    printf("[INIT] ✓ Request Handler initialized\n");

    // 2. Initialize Bandwidth Estimator
    bw_estimator_t bw_estimator;
    ret = bw_estimator_init(&bw_estimator, BW_ESTIMATOR_HARMONIC); // Using harmonic mean estimator
    if (ret != RET_SUCCESS) {
        printf("[ERROR] Failed to initialize bandwidth estimator\n");
        request_handler_destroy(&request_handler); // Clean up previously initialized components
        return 1;
    }
    printf("[INIT] ✓ Bandwidth Estimator initialized\n");

    // 3. Allocate Bandwidth History array
    bw_t *bw_history = (bw_t *)calloc(HISTORY_SIZE, sizeof(bw_t));
    if (!bw_history) {
        printf("[ERROR] Failed to allocate bandwidth history\n");
        request_handler_destroy(&request_handler);
        bw_estimator_destroy(&bw_estimator);
        return 1;
    }
    size_t bw_history_count = 0; // Keep track of the number of samples in history
    printf("[INIT] ✓ Bandwidth History allocated\n");

    // 4. Allocate Viewport Prediction History arrays
    float *yaw_history = (float *)calloc(HISTORY_SIZE, sizeof(float));
    float *pitch_history = (float *)calloc(HISTORY_SIZE, sizeof(float));
    int *timestamps = (int *)calloc(HISTORY_SIZE, sizeof(int));

    if (!yaw_history || !pitch_history || !timestamps) {
        printf("[ERROR] Failed to allocate viewport history\n");
        // Clean up allocated memory
        free(bw_history);
        free(yaw_history);
        free(pitch_history);
        free(timestamps);
        request_handler_destroy(&request_handler);
        bw_estimator_destroy(&bw_estimator);
        return 1;
    }

    // Initialize Viewport Predictor structure
    viewport_prediction_t vp_predictor = {
        .yaw_history = yaw_history,
        .pitch_history = pitch_history,
        .timestamps = timestamps,
        .current_index = 0, // Start index for circular buffer
        .sample_count = 0,  // Number of samples currently stored
        .next_timestamp = 0, // Timestamp for the next sample
        .history_size = HISTORY_SIZE,
        .vpes_post = vpes_legr // Set prediction function (linear regression)
    };
    printf("[INIT] ✓ Viewport Predictor initialized\n");

    // 5. Initialize Tile Selector
    tile_selection_t tile_selector;
    ret = tile_selection_init(&tile_selector, FIXED_TILE_SELECTION); // Using fixed tile selection logic
    if (ret != RET_SUCCESS) {
        printf("[ERROR] Failed to initialize tile selector\n");
        // Clean up allocated memory
        free(bw_history);
        free(yaw_history);
        free(pitch_history);
        free(timestamps);
        request_handler_destroy(&request_handler);
        bw_estimator_destroy(&bw_estimator);
        return 1;
    }
    printf("[INIT] ✓ Tile Selector initialized\n");

    // 6. Initialize ABR (Adaptive Bitrate) Selector
    abr_selector_t abr_selector;
    ret = abr_selector_init(&abr_selector, ABR_FOR_NORMAL_BUF); // Start with normal buffer logic
    if (ret != RET_SUCCESS) {
        printf("[ERROR] Failed to initialize ABR selector\n");
        // Clean up allocated memory
        free(bw_history);
        free(yaw_history);
        free(pitch_history);
        free(timestamps);
        request_handler_destroy(&request_handler);
        bw_estimator_destroy(&bw_estimator);
        return 1;
    }
    printf("[INIT] ✓ ABR Selector initialized\n");

    // Allocate storage for metrics per segment
    SegmentMetrics *all_metrics = (SegmentMetrics *)calloc(TOTAL_SEGMENTS, sizeof(SegmentMetrics));
    if (!all_metrics) {
        printf("[ERROR] Failed to allocate metrics storage\n");
        // Clean up allocated memory
        free(bw_history);
        free(yaw_history);
        free(pitch_history);
        free(timestamps);
        request_handler_destroy(&request_handler);
        bw_estimator_destroy(&bw_estimator);
        return 1;
    }

    // Initial state variables
    float buffer_level = INITIAL_BUFFER_SIZE; // Player buffer level in seconds
    int last_quality = 2; // Start with medium quality (index 2)
    bw_t predicted_bw = 5000000; // Initial bandwidth estimate: 5 Mbps

    printf("[INIT] ✓ All components initialized successfully\n\n");

    // ============================================
    // MAIN STREAMING LOOP
    // ============================================
    printf("[STREAMING] Starting main loop...\n\n");

    // Loop through each segment to be streamed
    // CHANGED: Reverted to 0-based loop to match server logs and file names
    for (COUNT segment_id = 0; segment_id <= TOTAL_SEGMENTS; segment_id++) {
        printf("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
        printf("  PROCESSING SEGMENT %llu / %d\n", segment_id, TOTAL_SEGMENTS); // Print 0-9
        printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

        // CHANGED: Use segment_id directly for 0-based array indexing
        SegmentMetrics *metrics = &all_metrics[segment_id];
        metrics->segment_id = segment_id; // Store the actual segment ID (0-9)
        metrics->buffer_level = buffer_level; // Record buffer level at the start of the segment

        // ----------------------------------------
        // STEP 1: Viewport Prediction
        // ----------------------------------------
        printf("\n[STEP 1] Viewport Prediction\n");

        // Add current viewport sample from the mock dataset to history
        // CHANGED: Use segment_id directly for 0-based array indexing
        if (segment_id < DATASET_SIZE) {
            add_viewport_sample(&vp_predictor,
                              viewport_dataset[segment_id].yaw,
                              viewport_dataset[segment_id].pitch);
            printf("  Added sample: yaw=%.2f°, pitch=%.2f°\n",
                   viewport_dataset[segment_id].yaw,
                   viewport_dataset[segment_id].pitch);
        }

        // Predict the viewport for the next segment
        float predicted_yaw = 0.0f, predicted_pitch = 0.0f;
        if (vp_predictor.sample_count >= 2) { // Need at least 2 samples for linear regression
            ret = vpes_legr(&vp_predictor, &predicted_yaw, &predicted_pitch);
            if (ret == RET_SUCCESS) {
                printf("  Predicted viewport: yaw=%.2f°, pitch=%.2f°\n",
                       predicted_yaw, predicted_pitch);
            } else {
                // Fallback to default if prediction fails
                printf("  Using default viewport (0°, 0°)\n");
                predicted_yaw = 0.0f;
                predicted_pitch = 0.0f;
            }
        } else {
            // Use default if not enough samples yet
            printf("  Insufficient samples, using default (0°, 0°)\n");
            predicted_yaw = 0.0f;
            predicted_pitch = 0.0f;
        }

        // Store predicted viewport in metrics
        metrics->predicted_yaw = predicted_yaw;
        metrics->predicted_pitch = predicted_pitch;

        // Select tiles based on the predicted viewport
        int viewport_tiles[NO_OF_ROWS * NO_OF_COLS]; // Array to store IDs of selected tiles
        int num_viewport_tiles = 0; // Number of tiles selected
        tile_selector.select_viewport(predicted_yaw, predicted_pitch,
                                      viewport_tiles,
                                      NO_OF_ROWS * NO_OF_COLS, // Max possible tiles
                                      &num_viewport_tiles);

        // Print selected viewport tiles
        printf("  Selected %d viewport tiles: [", num_viewport_tiles);
        for (int i = 0; i < num_viewport_tiles; i++) {
            printf("%d%s", viewport_tiles[i],
                   (i < num_viewport_tiles - 1) ? ", " : "");
        }
        printf("]\n");
        metrics->num_viewport_tiles = num_viewport_tiles;

        // ----------------------------------------
        // STEP 2: Bandwidth Estimation
        // ----------------------------------------
        printf("\n[STEP 2] Bandwidth Estimation\n");

        // Estimate bandwidth based on history
        if (bw_history_count > 0) {
            ret = bw_estimator_post_harmonic_mean(bw_history,
                                                  bw_history_count,
                                                  &predicted_bw);
            if (ret == RET_SUCCESS) {
                printf("  Harmonic mean BW: %.2f Mbps (from %zu samples)\n",
                       predicted_bw / 1000000.0, bw_history_count);
            } else {
                // Use previous estimate if calculation fails
                printf("  Using previous estimate: %.2f Mbps\n",
                       predicted_bw / 1000000.0);
            }
        } else {
            // Use initial estimate if no history yet
            printf("  No history yet, using initial estimate: %.2f Mbps\n",
                   predicted_bw / 1000000.0);
        }

        metrics->predicted_bandwidth = predicted_bw; // Store estimate in metrics

        // ----------------------------------------
        // STEP 3: ABR Decision (Quality Selection)
        // ----------------------------------------
        printf("\n[STEP 3] ABR Quality Selection\n");

        // Adjust ABR strategy based on current buffer level
        if (buffer_level < B_MIN) {
            printf("  Buffer DANGER (%.2fs < %.2fs) - Conservative mode\n",
                   buffer_level, B_MIN);
            abr_selector_init(&abr_selector, ABR_FOR_DANGER_BUF); // Switch to conservative ABR
        } else if (buffer_level >= B_HIGH) {
            printf("  Buffer HIGH (%.2fs >= %.2fs) - Aggressive mode\n",
                   buffer_level, B_HIGH);
            abr_selector_init(&abr_selector, ABR_FOR_HIGH_BUF); // Switch to aggressive ABR
        } else {
            printf("  Buffer NORMAL (%.2fs to %.2fs) - Normal mode\n",
                   buffer_level, B_NORMAL);
            abr_selector_init(&abr_selector, ABR_FOR_NORMAL_BUF); // Use normal ABR
        }

        // ORIGINAL ABR LOGIC - Commented out for debugging
        // int chosen_quality = abr_selector.choose_bitrate(
        //     (float)predicted_bw,
        //     PREDICTION_WINDOW,
        //     buffer_level,
        //     last_quality
        // );
        int chosen_quality = bitrate_fixed[segment_id];

        // // CHANGED: Force quality 4 (QP20) to match available files on server
        // int chosen_quality = 4;
        printf("  [DEBUG] Forcing quality 4 (QP20) to match server files.\n");


        printf("  Chosen quality: %d (QP: %d)\n",
               chosen_quality, tile_version_to_num(chosen_quality));
        printf("  Previous quality: %d\n", last_quality);

        metrics->chosen_quality = chosen_quality; // Store chosen quality in metrics

        // Prepare an array containing the chosen quality for each viewport tile
        int *chosen_versions = (int *)malloc(num_viewport_tiles * sizeof(int));
        for (int i = 0; i < num_viewport_tiles; i++) {
            chosen_versions[i] = chosen_quality;
        }

        // ----------------------------------------
        // STEP 4: Download Tiles
        // ----------------------------------------
        printf("\n[STEP 4] Downloading Tiles\n");

        // Measure download time
        struct timespec start_time, end_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time);

        // Request and download the selected tiles at the chosen quality
        // NO CHANGE: segment_id is passed directly (0-9)
        ret = request_handler_post_get_info(&request_handler,
                                            segment_id,
                                            viewport_tiles,
                                            num_viewport_tiles,
                                            chosen_versions);

        clock_gettime(CLOCK_MONOTONIC, &end_time);

        // Check if download was successful
        if (ret != RET_SUCCESS) {
            printf("  [ERROR] Download failed for segment %llu\n", segment_id);
            free(chosen_versions); // Clean up allocated memory
            continue; // Skip to the next segment
        }

        // ----------------------------------------
        // STEP 5: Update Metrics & State
        // ----------------------------------------
        printf("\n[STEP 5] Updating State\n");

        // Calculate download performance metrics
        double download_time_ms = (end_time.tv_sec - start_time.tv_sec) * 1000.0 +
                                 (end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;

        size_t total_bytes = 0; // Total bytes downloaded in this segment
        double avg_speed = 0;   // Average download speed for this segment
        int tile_count = 0;     // Number of tiles successfully downloaded

        // Iterate through download results for each tile
        for (COUNT i = 0; i < request_handler.tile_count; i++) {
            if (request_handler.size_dl[i] > 0) { // Check if tile was downloaded
                total_bytes += request_handler.size_dl[i];
                avg_speed += request_handler.dls[i]; // Add individual tile speed
                tile_count++;

                // Update bandwidth history (circular buffer)
                if (bw_history_count < HISTORY_SIZE) {
                    bw_history[bw_history_count++] = request_handler.dls[i];
                } else {
                    // Shift history and add new sample
                    memmove(bw_history, bw_history + 1, (HISTORY_SIZE - 1) * sizeof(bw_t));
                    bw_history[HISTORY_SIZE - 1] = request_handler.dls[i];
                }
            }
        }

        // Calculate average speed if any tiles were downloaded
        if (tile_count > 0) {
            avg_speed /= tile_count;
        }

        // Store performance metrics for the segment
        metrics->total_download_time_ms = download_time_ms;
        metrics->avg_download_speed_mbps = avg_speed / 1000000.0;
        metrics->total_bytes_downloaded = total_bytes;

        // Update buffer level: Decrease by download time, increase by segment duration
        float download_time_sec = download_time_ms / 1000.0;
        float old_buffer_level = buffer_level; // Store old level for printing
        buffer_level = buffer_level - download_time_sec + SEGMENT_DURATION;
        buffer_level = (buffer_level < 0) ? 0 : buffer_level; // Clamp buffer at 0
        buffer_level = (buffer_level > MAX_BUFFER_SIZE) ? MAX_BUFFER_SIZE : buffer_level; // Clamp buffer at max size

        // Print updated state information
        printf("  Download time: %.2f ms\n", download_time_ms);
        printf("  Average speed: %.2f Mbps\n", avg_speed / 1000000.0);
        printf("  Total data: %.2f MB\n", total_bytes / 1048576.0);
        printf("  Buffer level: %.2f -> %.2f seconds\n",
               old_buffer_level, buffer_level);

        // Update state for the next iteration
        last_quality = chosen_quality;

        // Print detailed metrics for the current segment
        print_segment_metrics(metrics);

        // Reset request handler's internal state for the next segment
        request_handler_reset(&request_handler);

        free(chosen_versions); // Clean up allocated memory for chosen versions

        // Check for potential rebuffering (buffer close to empty)
        if (buffer_level < 0.1f) {
            printf("\n[WARNING] Buffer underrun detected! Pausing...\n");
            // In a real system, playback would pause here until buffer refills
        }
    } // End of main streaming loop

    // ============================================
    // CLEANUP & SUMMARY
    // ============================================
    printf("\n\n[CLEANUP] Streaming completed, cleaning up...\n");

    // Print the summary of the entire streaming session
    print_summary(all_metrics, TOTAL_SEGMENTS);

    // Destroy and free all components and allocated memory
    request_handler_destroy(&request_handler);
    bw_estimator_destroy(&bw_estimator);
    free(bw_history);
    free(yaw_history);
    free(pitch_history);
    free(timestamps);
    free(all_metrics);

    printf("[CLEANUP] ✓ All resources released\n");
    printf("\nStreaming session ended successfully!\n\n");

    return 0; // Indicate successful execution
}


