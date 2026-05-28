#!/bin/bash

# ==========================================
# 1. KIỂM TRA QUYỀN ROOT
# ==========================================
if [ "$EUID" -ne 0 ]; then 
    echo "ERROR: Please run as root (sudo)"
    exit 1
fi

# ==========================================
# 2. CẤU HÌNH BIẾN
# ==========================================
CLIENT_PROGRAM="python3 ../run_client/run.py ../run_client/report_bicycle_0002_fake.log -- ../build_slr/my_program"

PIDSTAT_LOG="client_stats_http2_loss.log"
PERF_DATA_FILE="perf.data.http2_loss"
ENERGY_LOG="energy.log"
CPU_LOG="cpu_usage.log"

PIDSTAT_INTERVAL=1
PERF_FREQ=999

PKG="/sys/class/powercap/intel-rapl:0/energy_uj"
CORE="/sys/class/powercap/intel-rapl:0:0/energy_uj"
DRAM="/sys/class/powercap/intel-rapl:0:1/energy_uj"
MAX_RANGE="/sys/class/powercap/intel-rapl:0/max_energy_range_uj"

if [ ! -f "$PKG" ]; then
    echo "ERROR: RAPL not supported on this machine!"
    exit 1
fi

read_energy() {
    local before=$1
    local after=$2
    local max_range=$(cat $MAX_RANGE)
    if [ "$after" -lt "$before" ]; then
        echo $((after + max_range - before))
    else
        echo $((after - before))
    fi
}

# ==========================================
# 3. CHẠY CLIENT & PROFILERS
# ==========================================
echo "Starting client program..."
taskset -c 3 $CLIENT_PROGRAM &
CLIENT_PID=$!
echo "Client started with PID: $CLIENT_PID"

# Đợi 1 chút để process khởi tạo hoàn tất
sleep 0.5 

echo "Starting pidstat & perf..."
pidstat -u -p $CLIENT_PID $PIDSTAT_INTERVAL > $PIDSTAT_LOG &
PIDSTAT_PID=$!

perf record -F $PERF_FREQ -g -p $CLIENT_PID >/dev/null 2>&1 &
PERF_PID=$!

# Lấy thông số Năng lượng TRƯỚC khi xử lý
PKG_BEFORE=$(cat $PKG)
CORE_BEFORE=$(cat $CORE)
DRAM_BEFORE=$(cat $DRAM)

# ==========================================
# 4. ĐỢI CLIENT KẾT THÚC
# ==========================================
wait $CLIENT_PID
echo "Client program finished."

# Lấy thông số Năng lượng SAU khi xử lý
PKG_AFTER=$(cat $PKG)
CORE_AFTER=$(cat $CORE)
DRAM_AFTER=$(cat $DRAM)

# ==========================================
# 5. DỌN DẸP PROFILERS
# ==========================================
kill $PIDSTAT_PID 2>/dev/null
# Đợi perf ghi dữ liệu ra đĩa (dùng SIGINT để perf dừng gracefully)
kill -2 $PERF_PID 2>/dev/null
wait $PERF_PID 2>/dev/null
sleep 0.5

# Đổi tên file perf
if [ -f "perf.data" ]; then
    mv perf.data $PERF_DATA_FILE
    chmod 666 $PERF_DATA_FILE $PIDSTAT_LOG
else
    echo "WARNING: perf.data was not generated!"
fi

# ==========================================
# 6. TÍNH TOÁN & XUẤT KẾT QUẢ
# ==========================================
PKG_USED=$(read_energy $PKG_BEFORE $PKG_AFTER)
CORE_USED=$(read_energy $CORE_BEFORE $CORE_AFTER)
DRAM_USED=$(read_energy $DRAM_BEFORE $DRAM_AFTER)

echo "===== CPU ENERGY (microjoules) =====" > $ENERGY_LOG
echo "Package: $PKG_USED" >> $ENERGY_LOG
echo "Core:    $CORE_USED" >> $ENERGY_LOG
echo "DRAM:    $DRAM_USED" >> $ENERGY_LOG

echo "" >> $ENERGY_LOG
echo "===== CPU ENERGY (Joules) =====" >> $ENERGY_LOG
LC_NUMERIC=C printf "Package: %.6f J\n" "$(echo "$PKG_USED / 1000000" | LC_NUMERIC=C bc -l)" >> $ENERGY_LOG
LC_NUMERIC=C printf "Core:    %.6f J\n" "$(echo "$CORE_USED / 1000000" | LC_NUMERIC=C bc -l)" >> $ENERGY_LOG
LC_NUMERIC=C printf "DRAM:    %.6f J\n" "$(echo "$DRAM_USED / 1000000" | LC_NUMERIC=C bc -l)" >> $ENERGY_LOG

# Tính trung bình CPU
grep -E "^[0-9]" $PIDSTAT_LOG | awk '{print $1" "$7" "$8}' > $CPU_LOG.raw
USER_AVG=$(awk '{sum+=$2} END {if(NR>0) print sum/NR; else print 0}' $CPU_LOG.raw)
SYS_AVG=$(awk '{sum+=$3} END {if(NR>0) print sum/NR; else print 0}' $CPU_LOG.raw)

echo "===== AVERAGE CPU USAGE =====" > $CPU_LOG
LC_NUMERIC=C printf "User space:   %.2f %%\n" "$USER_AVG" >> $CPU_LOG
LC_NUMERIC=C printf "Kernel space: %.2f %%\n" "$SYS_AVG" >> $CPU_LOG

# Xóa file raw tạm
rm -f $CPU_LOG.raw

echo "======================================"
echo "Done."
echo "Energy log:   $ENERGY_LOG"
echo "CPU usage:    $CPU_LOG"
echo "pidstat log:  $PIDSTAT_LOG"
echo "perf data:    $PERF_DATA_FILE"
echo "======================================"