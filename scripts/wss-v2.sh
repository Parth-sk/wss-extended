\./workload &
pid=$!

sleep 0.5
./wss-v2 $pid 0.5

wait $pid