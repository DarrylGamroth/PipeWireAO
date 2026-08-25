\page page_polling_remote_graph_benchmark Exported-node graph scheduling benchmark

# Exported-node graph scheduling benchmark

## Result

The exported-node graph confirms that polling is an effective opt-in latency
policy and that eventfd remains appropriate when dedicating a CPU is not
acceptable. With source and sink on separate physical cores under
`SCHED_FIFO(88)`, two busy-spin loops delivered a 0.731 microsecond median and
2.946 microsecond p99.9 trigger-to-sink time. Two eventfd loops delivered
11.561 microseconds median and 20.278 microseconds p99.9. A mixed graph pays the
wakeup cost only at its eventfd boundary.

The hardware counters and cycle callgraphs do not identify a cache, page,
translation, or scheduler function that should be optimized. No measured cell
faulted or migrated. Huge pages are not justified for the scheduler working
set. The useful next measurement is the production camera-to-algorithm-to-AO
graph, not another synthetic scheduler rewrite.

## Measurement boundary

The benchmark creates two exported `pw_stream` nodes in one client process and
links them through an isolated PipeWireAO daemon. Source and sink always use
separate client data loops, even when they have the same idle policy. This
avoids the single-loop shortcut used by the functional remote test and makes
each readiness boundary independently selectable.

```text
main-loop trigger
       |
       | shared exported-node activation
       v
source data loop (CPU 12)
       |
       | ordinary PipeWire buffer and scheduler dependency
       v
sink data loop (CPU 14)
```

Every frame carries its sequence, trigger timestamp, and source-callback
timestamp in the ordinary mapped PipeWire buffer. The sink records:

- trigger to source callback, which isolates source-loop readiness;
- source callback to sink callback, which includes publication, dependency
  propagation, and sink-loop readiness; and
- trigger to sink callback, which is the complete measured graph transit.

`CLOCK_MONOTONIC_RAW` timestamps are read at the trigger, source, and sink. The
small source and sink test callbacks and timestamp calls are included. Stream
construction, link negotiation, process startup, output, sorting, and warmup
are excluded.

The graph is remote from its daemon and exercises the exported-node shared
activation and module-client-node path. Both callbacks are in one client
process; this is not an inter-client, network, camera, algorithm, AO-device, or
multi-host measurement. The 100 microsecond trigger timer paces attempts, while
`trigger_done` permits only one frame in flight. Latency starts at the actual
trigger call, so timer wakeup is outside the measurement.

## Latency campaign

The recorded campaign used:

- AMD Ryzen 7 6800H and Linux 6.12.57 with full preemption;
- source CPU 12 and sink CPU 14, which are separate physical cores in one
  package;
- `SCHED_OTHER` and `SCHED_FIFO(88)`;
- all four source/sink eventfd and busy-spin combinations;
- 2,000 warmup frames and 10,000 recorded frames per cell;
- five randomized repetitions, 40 cells, and 400,000 recorded frames;
- pre-faulted result storage; and
- exact raw samples rather than reservoir sampling.

The CPUs were not kernel-isolated, SMT siblings remained online, and the
frequency governor remained `powersave` with the energy preference set to
`performance`. Results are evidence from this development host, not a
hardware-independent worst-case bound.

Values below are the median statistic across five repetitions. The maximum is
the median of five per-repetition maxima.

| Source | Sink | Scheduler | Source p50 | Link p50 | Total p50 | Total p99 | Total p99.9 | Median maximum |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| busy-spin | busy-spin | OTHER | 0.360 us | 0.370 us | 0.722 us | 1.353 us | 2.935 us | 4.779 us |
| busy-spin | busy-spin | FIFO 88 | 0.351 us | 0.371 us | 0.731 us | 1.283 us | 2.946 us | 6.392 us |
| busy-spin | eventfd | OTHER | 0.351 us | 7.554 us | 7.925 us | 10.820 us | 35.176 us | 222.477 us |
| busy-spin | eventfd | FIFO 88 | 0.331 us | 5.921 us | 6.281 us | 9.097 us | 13.425 us | 19.206 us |
| eventfd | busy-spin | OTHER | 5.491 us | 0.511 us | 6.001 us | 10.540 us | 36.298 us | 524.984 us |
| eventfd | busy-spin | FIFO 88 | 6.041 us | 0.611 us | 6.683 us | 9.328 us | 13.575 us | 20.819 us |
| eventfd | eventfd | OTHER | 4.588 us | 7.544 us | 12.043 us | 18.113 us | 51.908 us | 187.873 us |
| eventfd | eventfd | FIFO 88 | 5.169 us | 6.371 us | 11.561 us | 14.578 us | 20.278 us | 32.190 us |

The complete samples, run order, executable hashes, source diff, environment,
and system snapshots are in the
[latency record](benchmark-data/polling-remote-graph-2026-08-25-record/).
Latency CSVs are individually Zstandard-compressed. `RAW_SHA256SUMS` verifies
their original contents and `SHA256SUMS` verifies the committed archives.

## CPU, cache, TLB, and fault counters

The PMU campaign repeated all four FIFO 88 modes for five event groups and five
randomized repetitions: 100 cells and 500,000 measured frames. Each cell used
1,000 warmup and 5,000 measured frames. Perf attached to both warmed data-loop
TIDs and was enabled and disabled through a four-pipe gate. Every hardware
counter had 100 percent scheduling coverage.

Values are medians across five repetitions. Counts are the combined source and
sink loop totals per frame.

| Source | Sink | Cycles | Instructions | IPC | L1D misses | L2 misses | L2 DTLB misses | Context switches per 5k |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| busy-spin | busy-spin | 865,712 | 1,630,631 | 1.882 | 50.06 | 39.87 | 0.376 | 0 |
| busy-spin | eventfd | 438,334 | 821,280 | 1.874 | 76.99 | 37.04 | 1.569 | 5,000 |
| eventfd | busy-spin | 439,135 | 818,695 | 1.864 | 136.71 | 42.52 | 1.237 | 9,935 |
| eventfd | eventfd | 7,581 | 3,757 | 0.496 | 188.36 | 47.44 | 9.281 | 15,000 |

At the 100 microsecond pacing interval, each polling loop intentionally spends
its otherwise idle core scanning. Two polling loops therefore retire about
twice the cycles and instructions of one polling loop. These totals are CPU
occupancy per frame, not the incremental cost of processing a frame. The
eventfd loops sleep, use much less user CPU, and incur the expected context
switches and kernel wake latency.

All 20 fault-group cells recorded zero minor faults, zero major faults, and
zero CPU migrations during their warmed gate. Busy-spin L1D miss rates were
below 0.01 percent. The larger eventfd miss rates are percentages of a much
smaller number of user-space loads after rescheduling. Absolute L2 and DTLB
miss counts remain too small to explain microsecond wake latency.

The complete event counts, coverage, and latency samples are in the
[PMU record](benchmark-data/polling-remote-graph-pmu-2026-08-25-record/).
It uses the same compressed-CSV and two-manifest convention.

## Hot functions

Lossless `cycles:u` callgraphs were recorded at 999 Hz with 8 KiB DWARF stacks
for 10,000 frames in every FIFO 88 mode. Raw `perf.data` files are not retained;
the text self/children reports and build IDs are in the
[cycle record](benchmark-data/polling-remote-graph-cycles-2026-08-25-record/).
Because two sleeping eventfd loops accumulated only 285 user-cycle samples in
that interval, a separate 200,000-frame
[eventfd cycle record](benchmark-data/polling-remote-graph-eventfd-cycles-2026-08-25-record/)
provides 494 lossless samples for that mode.

With two polling loops, `do_poll_loop` accounts for 33.81 percent of combined
samples on the source thread and 23.31 percent on the sink thread. The next
visible paths are the required `spa_loop_control_yield()` handoff,
`loop_yield`, `flush_all_queues`, and `poll_process_node`. With one polling
loop, that loop dominates the combined source-plus-sink user cycles, as
expected.

With two eventfd loops, no PipeWire function dominates. In the longer record,
`node_ready` is the largest named self entry at 5.35 percent,
`trigger_target_v2` contributes 1.52 percent on the source and 1.18 percent on
the sink, and the instrumented test callbacks and vDSO clocks are also visible.
This is a distributed wake and
activation path, not evidence for optimizing one hot function.

The profile reinforces the earlier empty-scan result: removing the yield point
or batching control work would trade control-plane fairness for idle-loop
cycles without addressing eventfd latency. No such change is recommended.

## Memory policy and huge pages

The benchmark pre-faults result storage and warms mapped buffers and scheduler
state before measurement. PipeWire's default `mem.allow-mlock=true` asks stream
and exported-node code to lock mapped buffer memory when the process has an
adequate `RLIMIT_MEMLOCK`. This record did not independently verify every
successful `mlock`, so deployment qualification should enable
`mem.warn-mlock=true` and check the logs and `/proc/*/status` `VmLck` value.

For a dedicated RTC process, `mem.mlock-all=true` is available and calls
`mlockall(MCL_CURRENT | MCL_FUTURE)` during context construction. Use it only
with a deliberately sized memlock limit and after qualifying process memory
growth; an insufficient limit can make later allocations fail. Its value is
protection from reclaim and first-use faults, not a demonstrated reduction in
this warmed service time.

Huge pages do not help this scheduler path. Activations are cache-line-sized,
the payload is 24 bytes, the mapped buffers are small, warmed faults are zero,
and translation misses are negligible. Huge pages should be reconsidered only
for a large algorithm allocation whose own counter profile shows a translation
bottleneck, not for PipeWire activation or eventfd state.

## Reproduction

Build the daemon, link tool, and `pw-test-polling-remote-client`, then run:

```console
python3 src/tests/benchmark-polling-remote.py \
    build/src/daemon/pipewire-ao \
    build/src/tools/pwao-link \
    build/src/tests/pw-test-polling-remote-client \
    remote-latency-results \
    --repetitions 5 --samples 10000 --warmup 2000 \
    --interval-ns 100000 --rt-priority 0 --rt-priority 88

python3 src/tests/benchmark-polling-remote.py \
    build/src/daemon/pipewire-ao \
    build/src/tools/pwao-link \
    build/src/tests/pw-test-polling-remote-client \
    remote-pmu-results \
    --repetitions 5 --samples 5000 --warmup 1000 \
    --interval-ns 100000 --rt-priority 88 \
    --group core --group l1d --group l2 --group dtlb --group faults

python3 src/tests/benchmark-polling-remote.py \
    build/src/daemon/pipewire-ao \
    build/src/tools/pwao-link \
    build/src/tests/pw-test-polling-remote-client \
    remote-cycle-results \
    --repetitions 1 --samples 10000 --warmup 2000 \
    --interval-ns 100000 --rt-priority 88 --sample-cycles
```

The runner selects two physical cores when CPU numbers are omitted. It rejects
wrong affinity, wrong scheduling policy or priority, migrations, multiplexed
counters, lost cycle samples, incomplete transfers, and malformed latency
files.
