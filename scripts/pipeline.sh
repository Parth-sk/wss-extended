#!/bin/bash
# run_profiler.sh
# Usage: ./run_profiler.sh <workload> <wss_duration> <wss_intervals>

WORKLOAD=$1
WSS_DURATION=$2
WSS_INTERVALS=$3

if [ -z "$WORKLOAD" ] || [ -z "$WSS_DURATION" ] || [ -z "$WSS_INTERVALS" ]; then
    echo "Usage: $0 <workload> <wss_duration> <wss_intervals>"
    exit 1
fi

WORKLOAD_NAME=$(basename $WORKLOAD)

# launch workload under perf mem in background
sudo perf mem record -g -o perf.data $WORKLOAD &
PERF_PID=$!
echo "Launched $WORKLOAD under perf mem (perf PID $PERF_PID)"

# poll until workload process appears - up to 10 seconds
echo "Waiting for $WORKLOAD_NAME to start..."
PID=""
for i in $(seq 1 20); do
    sleep 0.5
    PID=$(pgrep -x $WORKLOAD_NAME)
    if [ -n "$PID" ]; then
        break
    fi
done

if [ -z "$PID" ]; then
    echo "Error: could not find PID for $WORKLOAD_NAME after 10 seconds"
    echo "Running processes:"
    ps aux | grep -i mult
    kill $PERF_PID 2>/dev/null
    exit 1
fi
echo "Found workload PID $PID"


# run wss for the full measurement duration, output to file
sudo ../src/wss/wss-new $PID $WSS_DURATION $WSS_INTERVALS > wss_output.txt
echo "WSS complete, output in wss_output.txt"

# wait for perf/workload to finish
wait $PERF_PID 2>/dev/null
echo "perf mem finished"

# extract perf samples

sudo perf script -F time,addr,period,ip,sym,callindent > perf_output.txt
echo "perf script output in perf_output.txt"

echo ""
echo "Run: python3 analyze.py wss_output.txt perf_output.txt --top 5"