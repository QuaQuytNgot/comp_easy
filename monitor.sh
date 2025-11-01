#!/bin/bash

# --- Configuration ---
# Set the name of your client executable
CLIENT_PROGRAM="./build/my_program"

# Set the output log file names
PIDSTAT_LOG="client_stats_http2.log"
PERF_DATA_FILE="perf.data.http2" # Change this for http2 tests

# --- Test Execution ---
echo "Starting client program in the background..."
# Launch the client program and run it in the background (&)
# Then, immediately capture its Process ID (PID)
$CLIENT_PROGRAM &
CLIENT_PID=$!

echo "Client started with PID: $CLIENT_PID"
echo "Starting background monitoring with pidstat..."

# Start pidstat, telling it to monitor ONLY our client's PID
pidstat -u -r -p $CLIENT_PID 1 > $PIDSTAT_LOG &
PIDSTAT_PID=$!

echo "Attaching perf to monitor the client program..."
# Attach perf record to the running client PID.
# This also runs in the background.
perf record -g -p $CLIENT_PID &
PERF_PID=$!

# This is the key step: The script will now pause and wait for your
# client program to finish its execution.
wait $CLIENT_PID

echo "Client program finished. Stopping monitoring tools..."

# Clean up the monitoring processes now that the client is done.
kill $PERF_PID
kill $PIDSTAT_PID

# Rename the perf.data file to keep it organized.
mv perf.data $PERF_DATA_FILE

echo "Monitoring complete. Logs are in $PIDSTAT_LOG and $PERF_DATA_FILE"