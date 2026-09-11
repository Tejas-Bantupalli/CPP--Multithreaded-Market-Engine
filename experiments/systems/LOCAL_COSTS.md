# Supplemental uncontended queue costs

The concurrent pilot's sentinel showed large scheduling noise. `local_cost.cpp`
therefore provides a separate foundation measurement: one thread immediately
pushes then pops from an empty queue. It validates every sequence and checksum.
This is **not a cross-thread latency test**, has no consumer wakeup, and says
nothing directly about contention or cache-line transfer between cores.

```sh
c++ -std=c++17 -O2 -pthread -Wall -Wextra -Iinclude \
  experiments/systems/local_cost.cpp -o build/systems_local_cost
./build/systems_local_cost spsc
./build/systems_local_cost mutex
./build/systems_local_cost mutex_cv
```

Each executable measures forty batches of 50,000 push/pop pairs. Records contain
mean nanoseconds per pair for each batch, separately for thread CPU and wall time.
These are batch-average operation costs, **not per-message p50/p99 latencies**.
The CV variant still issues notifications, but no consumer is blocked on it.

The pilot ran each variant three times in shuffled order, retaining all batch
records in `local-costs.json` and compile/source/binary provenance in
`local-costs-metadata.json`. The main concurrent matrix and its frozen source
were not changed or rerun as a result of this supplemental measurement.
