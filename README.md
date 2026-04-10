# Coordinated Tile Streaming (CTS) Pipeline over HTTP/3

## 📖 Introduction
This repository contains the source code for a **Viewport-Adaptive 360-Degree Video Streaming** system, specifically optimized for **HTTP/3** and **HTTP/2** protocols. 

The primary highlight of this system is its approach to solving the "User-space Overhead" bottleneck caused by the heavy cryptographic processing of HTTP/3 (QUIC/TLS). The system achieves this through two core mechanisms:
1. **Cross-Layer Dynamic Pacing**.
2. **Reactive Early Termination**.

### ✨ Key Features:
* **Viewport-Adaptive Streaming (VAS):** Fetches high-quality video representations only for the tiles located within the user's current field of view (viewport).
* **Priority-Aware Dynamic Pacing:** An intelligent CPU task scheduler that dynamically regulates the dispatch rate of HTTP/3 requests based on the OS kernel pressure ($\rho_{sys}$). This flattens CPU spikes and prevents protocol bottlenecking.
* **Early Termination (ET):** Proactively aborts in-flight requests for tiles that fall out of the viewport due to sudden head movements. It uses `RESET_STREAM` (HTTP/3) or `RST_STREAM` (HTTP/2) frames to instantly reclaim bandwidth and computing resources.
* **QoE & System Profiling:** Built-in resource monitors (CPU, Network) and Quality of Experience (QoE) calculation based on academic standards.

---

## ⚙️ The CTS Pipeline
The streaming system operates in a continuous loop (Segment Loop) for each video segment:
1. **Resource Monitoring:** Reads system stress metrics (`rho_sys`, `C_proc`).
2. **Viewport Prediction:** Calculates the user's predicted viewing direction (yaw, pitch).
3. **Adaptive Logic:** Computes the dynamic drop threshold (`tau`) and probability map (`p_map`).
4. **Scheduling (SLR):** Determines the optimal bitrate for each tile using Lagrangian relaxation.
5. **Parallel Download (Pacer & ET):** * Dispatches high-priority requests (Viewport tiles) immediately.
   * Defers low-priority requests (Peripheral tiles) until the CPU has recovered from the initial TLS handshake burst.
   * Continuously polls active streams; if a tile's probability drops below `tau`, the stream is aborted instantly.
6. **Metrics Calculation:** Computes the Wasted Ratio, Smoothness, and QoE.

---

## 📂 Repository Structure
The project is modularized for easy maintenance and scalability:

```text
comp_easy/
├── CMakeLists.txt              # CMake build configuration
├── main.c                      # Entry point (Streaming Orchestrator)
├── include/
│   └── proto_comp/             # Header files (.h)
│       ├── abr.h               # Adaptive Bitrate baseline algorithms
│       ├── cts_scheduler.h     # CTS Scheduler (RA-MPC, Subgradient)
│       ├── http_pool.h         # cURL multi-handle and stream management
│       ├── request_handler_v2.h# Metrics aggregation, Wasted Ratio, QoE
│       ├── resource_monitor.h  # System resource monitoring (C_proc, rho_sys)
│       └── viewport_prediction.h # Viewport prediction algorithms
└── src/                        # Source files (.c)
    ├── cts_scheduler.c         # Bitrate allocation logic
    ├── http_pool.c             # Implementation of Dynamic Pacing & Early Termination
    ├── request_handler_v2.c    # QoE calculation and CSV logging
    ├── resource_monitor.c      # Kernel data extraction (via /proc/stat)
    └── viewport_prediction.c   # Viewport updating and calculation
```

---

## 🛠 Prerequisites & Building

### System Requirements
* **Operating System:** Linux (Ubuntu 20.04 or 22.04 recommended).
* **Compiler:** GCC & CMake (>= 3.10).
* **Network Library:** `libcurl` **must be built with HTTP/3 support** (Recommended backend: `wolfSSL` + `ngtcp2` or `quiche`).
* **Profiling Tool (Optional):** `perf` (`linux-tools-common`, `linux-tools-generic`).

### Server:

Server should contain 360 video tiling and encoding into 8x6 tiles.
For FILrg member: server is prepared in fil@192.168.101.107 if you use Fil LAN network.
From a computer in LAN:
```bash
ssh fil@192.168.101.17  #pass: 123456
cd /media/fil/VR/Hoang/360vprep/
bash h3_server.sh  #for HTTP/3 server, HTTP/2 command will be updated later.
```

### Build Instructions

NOTE: You should use absolute path to the built libcurl inside CMakeLists.txt (will be update so that this repo is ready to run in any computer).

From the root directory of the repository, run the following commands:
```bash
#build is the folder will contain executabel file
cmake -S . -B build
cmake --build build
```
Upon successful compilation, the executable (e.g., `my_program`) will be generated inside the `build/` directory.

---

## 🚀 Usage

To start the streaming simulation:
```bash
./buid/my_program
```

*During execution, the system will output detailed logs to the terminal:*
* `[RM]`: CPU resource status (`rho_sys`, `C_proc`).
* `[VP]`: Predicted viewport direction (yaw/pitch).
* `[TAU]`: The early termination threshold ($\tau$) and rapid movement (saccade) warnings.
* `[http_pool]`: Real-time stream abortion events (`EARLY-TERM` -> `RESET_STREAM`) and parallel task count.
* `[METRICS]`: Wasted Ratio, Smoothness, and Normalized QoE score for the current segment.

At the end of the session, the program automatically generates an `http_metrics_v2.csv` file containing detailed per-segment data for plotting and analysis.

---

## 🧩 Where to Implement New Features

For researchers looking to extend or customize the system, here is the development map:

* **Change the Adaptive Bitrate Algorithm:** Modify the `cts_schedule()` function in `src/cts_scheduler.c`. You can access bandwidth and CPU parameters via the `cts_input_t` struct.
* **Tweak Pacing & Early Termination:** Adjust the logic inside `http_pool_get_parallel_dynamic()` in `src/http_pool.c`. You can tweak the CPU safety threshold (`stress < 0.20f`) or the priority classification rule (`p >= 0.50f`).
* **Modify the QoE Formula:** Update `calculate_normalized_segment_qoe()` in `src/request_handler_v2.c`. You can adjust the weights ($\alpha, \beta, \gamma$) or normalization caps to fit a new dataset.
* **Upgrade Viewport Prediction:** Replace the default Linear Regression model in `src/viewport_prediction.c`. You can integrate ML libraries (like ONNX Runtime) to run LSTM or Deep Learning models here.

---

## 📊 Performance Profiling with `perf`

To evaluate and prove the reduction of User-space Overhead over HTTP/3, use the `perf` tool:

```bash
# 1. Automatically measuring, you should change CLIENT_PROGRAM field if need
sudo bash monitor.sh

# 2. View the detailed report
sudo perf report -i perf.data
```
*💡 Profiling Tip:* When analyzing the report, pay attention to the **Event count (Total Cycles)** at the top rather than just the relative percentages (%). The absolute drop in Total Cycles, alongside the reduced footprint of functions like `GHASH` or `do_recvmmsg`, acts as concrete proof that redundant cryptographic and I/O tasks have been successfully eliminated.

---
**Authors:** Future Internet Laboratory (Hanoi University of Science and Technology)  
**References:** Based on our incoming paper :>.