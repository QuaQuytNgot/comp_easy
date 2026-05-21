import subprocess
import time
import sys
import os

# Network interface of the Client machine connecting to the Server (e.g., eno1, wlan0, or enp3s0)
INTERFACE = "enp1s0" 
# Port of the HTTP Server
PORT = 8443

def setup_tc():
    print(f"Setting up Ingress Traffic Control (tc + ifb) on {INTERFACE}...")
    
    # 1. Load the IFB kernel module
    subprocess.run("sudo modprobe ifb numifbs=1", shell=True)
    subprocess.run("sudo ip link set dev ifb0 up", shell=True)
    
    # 2. Clean up old rules on both interfaces
    subprocess.run(f"sudo tc qdisc del dev {INTERFACE} handle ffff: ingress 2>/dev/null", shell=True)
    subprocess.run(f"sudo tc qdisc del dev ifb0 root 2>/dev/null", shell=True)
    
    # 3. Create an Ingress qdisc on the physical interface
    subprocess.run(f"sudo tc qdisc add dev {INTERFACE} handle ffff: ingress", shell=True)
    
    # 4. Redirect all packets from the Server (sport = PORT) coming into the interface to the egress of ifb0
    # This is the "magic" that converts Ingress to Egress for bandwidth shaping
    cmd_redirect = f"sudo tc filter add dev {INTERFACE} parent ffff: protocol ip u32 match ip sport {PORT} 0xffff action mirred egress redirect dev ifb0"
    subprocess.run(cmd_redirect, shell=True)
    
    # 5. Apply HTB limit on the egress of ifb0
    subprocess.run("sudo tc qdisc add dev ifb0 root handle 1: htb default 10", shell=True)
    subprocess.run("sudo tc class add dev ifb0 parent 1: classid 1:10 htb rate 1000mbit", shell=True)

def cleanup_tc():
    print(f"\nCleaning up Traffic Control (tc + ifb)...")
    subprocess.run(f"sudo tc qdisc del dev {INTERFACE} handle ffff: ingress 2>/dev/null", shell=True)
    subprocess.run(f"sudo tc qdisc del dev ifb0 root 2>/dev/null", shell=True)
    subprocess.run("sudo ip link set dev ifb0 down 2>/dev/null", shell=True)

def apply_rate(rate_kbps):
    # Change the bandwidth on the ifb0 interface (equivalent to throttling Client's download)
    cmd = f"sudo tc class change dev ifb0 parent 1: classid 1:10 htb rate {rate_kbps}kbit"
    subprocess.run(cmd, shell=True)

def read_mobility_log(log_file):
    """Read Mobility log file (Bus, Bicycle) and return a list of tuples: (Kbps, duration_seconds)"""
    trace_data = []
    
    with open(log_file, 'r') as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) >= 6:
                try:
                    bytes_transferred = float(parts[4])
                    duration_ms = float(parts[5])
                    
                    if duration_ms > 0:
                        kbps = int((bytes_transferred * 8) / duration_ms)
                        duration_sec = duration_ms / 1000.0
                        trace_data.append((kbps, duration_sec))
                except ValueError:
                    continue
                    
    return trace_data

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 run_mobility_emulator.py <file_report.log> [-- <client_command>]")
        sys.exit(1)
        
    client_cmd = []
    if "--" in sys.argv:
        separator_idx = sys.argv.index("--")
        log_file = sys.argv[1]
        client_cmd = sys.argv[separator_idx + 1:]
    else:
        log_file = sys.argv[1]

    print(f"Reading trace: {log_file}")
    
    trace_data = read_mobility_log(log_file)
    if not trace_data:
        print("Log file is empty or malformed!")
        return
        
    print(f"Successfully loaded {len(trace_data)} network simulation records.")
    setup_tc()
    
    client_process = None
    try:
        print("\nStarting Trace execution...")
        
        if client_cmd:
            print(f" Automatically launching client: {' '.join(client_cmd)}")
            client_process = subprocess.Popen(client_cmd)
        
        start_time = time.time()
        accumulated_time = 0.0
        
        for i, (rate, duration) in enumerate(trace_data):
            if client_process is not None and client_process.poll() is not None:
                print("\nClient has finished downloading the video! Automatically ending simulation early.")
                break

            safe_rate = max(10, rate) 
            apply_rate(safe_rate)
            
            print(f"[Record {i+1}/{len(trace_data)}] Download Bandwidth: {safe_rate} Kbps ({safe_rate/1000:.2f} Mbps) | Duration: {duration:.3f}s")
            
            accumulated_time += duration
            target_time = start_time + accumulated_time
            sleep_duration = target_time - time.time()
            
            if sleep_duration > 0:
                time.sleep(sleep_duration)
                
        if client_process is not None and client_process.poll() is None:
             print("\nTrace file ended, but the Client process is still running.")
             client_process.wait()

    except KeyboardInterrupt:
        print("\nInterrupted by user (Ctrl+C).")
        if client_process and client_process.poll() is None:
            client_process.terminate()
    finally:
        cleanup_tc()

if __name__ == "__main__":
    main()