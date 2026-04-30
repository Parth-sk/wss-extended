\./Workloads/workload &
pid=$!

sleep 0.5
./wss-v1 -C $pid 0.5
