\page page_polling_data_loop_profiling Polling data-loop profiling

# Polling data-loop profiling

## Questions

This profile investigates whether the warmed polling scheduler is limited by
instructions, branches, CPU caches, address translation, virtual-memory
faults, scheduling, or cache-line coherence. It supplements the latency
benchmark; PMU sampling is not used as a latency result because profiling
changes execution.

## Measurement boundary

The benchmark provides a profiling gate after warmup. At that gate it reports
the actual data-loop thread ID and pauses until `perf` has attached and
acknowledged event enablement. It signals again after the last measured sample,
then waits while the runner disables the counters. The histogram sort, file
output, process startup, allocation, page pre-faulting, and caller thread are
outside the counter scope.

The two pipe handshakes introduce one expected boundary context switch in most
scan counter cells. They do not occur between measured scans. Activation
context-switch counts therefore compare the two readiness mechanisms after
accounting for a fixed boundary event.

All PMU groups were scheduled at 100 percent without multiplexing. Hardware
events use the user-space privilege filter. Generic LLC events were not
supported by this AMD PMU, so the L2 group uses the locally reported Zen event
definitions. CPU cache misses, TLB misses, operating-system page faults, and
filesystem page-cache behavior are distinct; this experiment measures the
first three and makes no filesystem page-cache claim.

## Recorded campaigns

The campaigns ran on the same AMD Ryzen 7 6800H and Linux 6.12.57 development
host as the latency campaign. Caller CPU 12 and loop CPU 14 are different
physical cores. CPUs were not isolated, SMT and boost remained enabled, and
the frequency policy remained unchanged. `perf` 6.12.101 reported
`perf_event_paranoid=-1`.

The scan counter campaign contains 250 randomized cells:

- five repetitions;
- 1,000,000 warmup and 1,000,000 measured scans per cell;
- 0, 1, 4, 16, and 64 no-op work sources plus the timing probe;
- `SCHED_OTHER` and `SCHED_FIFO(88)`; and
- separate core, L1D, Zen L2, DTLB, and fault/scheduling groups.

The activation campaign contains 100 randomized cells:

- five repetitions;
- 100,000 warmup and 100,000 measured activations per cell;
- busy-spin and eventfd readiness;
- `SCHED_OTHER` and `SCHED_FIFO(88)`; and
- the same five counter groups on the consumer data-loop thread.

Cycle call graphs use `cycles:u` at 4,999 Hz with 8 KiB DWARF stacks for
5,000,000 measured scans at zero and 64 work sources. No samples were lost.
Raw `perf.data` files were not retained because they are host-specific binary
artifacts that can contain addresses and symbol data. The textual reports,
build IDs, commands, environment, and hashes are retained.

## Polling scan counters

Values are medians across five `SCHED_FIFO(88)` repetitions. The
`SCHED_OTHER` values are in the raw aggregate and lead to the same conclusion.

| Work sources | Cycles/scan | Instructions/scan | IPC | L1D misses/scan | L2 misses/scan | L2 DTLB misses/scan |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 180.85 | 306.00 | 1.692 | 0.1292 | 0.00422 | 0.00198 |
| 1 | 183.35 | 320.00 | 1.745 | 0.1290 | 0.00416 | 0.00198 |
| 4 | 198.40 | 362.00 | 1.825 | 0.1310 | 0.00493 | 0.00197 |
| 16 | 270.27 | 530.00 | 1.961 | 0.1310 | 0.00501 | 0.00199 |
| 64 | 662.76 | 1,202.00 | 1.814 | 0.1372 | 0.00660 | 0.00202 |

The instruction count is effectively `306 + 14 * work_sources`; the branch
count is `82 + 5 * work_sources`. Cache and TLB misses remain nearly constant
while the source count grows. The linked-list traversal and no-op indirect
calls are therefore executing from hot cache on this matrix. The 64-source
increment is about 7.5 cycles per work source and is not memory-latency bound.

Every warmed scan fault cell recorded zero minor faults, zero major faults, and
zero CPU migrations. This validates pre-faulting the result storage before
measurement.

## Cycle attribution

At zero work sources, the self-overhead report attributes:

- 46.91 percent to the timing probe's vDSO `clock_gettime`;
- 24.33 percent to `do_poll_loop`;
- 7.47 percent to `scan_probe`; and
- the remainder mainly to the control-loop yield path.

The children report attributes 25.37 percent through
`spa_loop_control_yield()`, including 4.95 percent in
`pthread_mutex_lock()` and 4.22 percent in `flush_all_queues()`. This is about
45 cycles per synthetic zero-source scan, not 25 percent of a production graph
cycle. The timing probe is benchmark instrumentation and is not scheduler
work.

At 64 work sources, `scan_work()` accounts for 63.57 percent, `do_poll_loop`
for 16.87 percent, and the timing clock for 12.46 percent. This agrees with the
linear instruction counts and does not reveal a cache-locality hotspot.

## Activation counters

Values are medians across five `SCHED_FIFO(88)` repetitions on the consumer
data-loop thread.

| Readiness | Cycles/activation | Instructions/activation | IPC | Branch-miss rate | L1D misses/activation | L2 misses/activation | L2 DTLB misses/activation | Context switches per 100k |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| busy-spin | 1,559.82 | 1,933.95 | 1.241 | 0.166% | 8.93 | 6.80 | 0.0021 | 0 |
| eventfd | 1,109.68 | 764.68 | 0.689 | 8.060% | 10.49 | 4.47 | 0.0865 | 99,723 |

Eventfd uses fewer consumer-thread user cycles because the thread sleeps, but
it incurs approximately one context switch per activation and its kernel wake
latency is not represented by `cycles:u`. Busy-spin spends additional user
cycles scanning so that it can avoid that wakeup. These counters explain the
mechanisms; the latency campaign remains authoritative for wake-to-process
latency.

Both modes recorded zero minor faults, zero major faults, and zero migrations.
Eventfd has more branch and TLB disruption after rescheduling. Busy-spin keeps
the instruction path and translations hot, but still transfers shared
activation state between the caller and consumer cores.

The activation layout places `status` at offset 0 and `signal_time` at offset
32 in the first cache line. The version-2 `signal_time_seq` compatibility field
is at offset 512 in another cache line. The measured L2 traffic makes
coherence a plausible contributor, but aggregate counters cannot identify the
specific line. On this AMD host, `perf c2c` maps `mem-ldst` to IBS and rejected
per-thread collection; it requires system-wide `-a`. A system-wide retry was
not run because it would collect unrelated processes. Run c2c only on isolated
target CPUs in a controlled qualification environment.

## Optimization decision

No production scheduler optimization is justified by this profile:

- the polling source walk is instruction-linear and cache-hot through 64
  sources;
- warmed scans and activations have no page faults or migrations;
- DTLB activity is negligible in busy-spin mode;
- the empty yield path costs only tens of cycles and preserves administrative
  lock progress and invoke-queue fairness; and
- the remaining activation cache traffic is expected cross-core publication,
  while address-level attribution is not yet available.

Skipping the mutex handoff or batching yields would exchange a small median
improvement for a new control-plane latency policy. Moving the activation
sequence field would break the shared activation layout. Neither change should
be made from these synthetic results.

An exported-node buffer graph has now been profiled with the same page-fault
gate; see [Exported-node graph scheduling benchmark](polling-remote-graph-benchmark.md).
It confirms the synthetic result and does not expose a scheduler hot function
that warrants optimization. The next useful profile is an end-to-end camera to
algorithm to AO graph on isolated deployment CPUs. That run should collect
per-node CPU budgets and—if system-wide collection is acceptable—use IBS/c2c
to verify the activation line and the queue ownership boundaries.

## Reproduction and data

Build `pw-test-polling-data-loop`, then run:

```console
python3 src/tests/profile-polling-data-loop.py \
    build/src/tests/pw-test-polling-data-loop scan-pmu-results

python3 src/tests/profile-polling-activation.py \
    build/src/tests/pw-test-polling-data-loop activation-pmu-results

python3 src/tests/sample-polling-data-loop.py \
    build/src/tests/pw-test-polling-data-loop cycle-sample-results
```

The complete records are in:

- [`benchmark-data/polling-data-loop-pmu-2026-08-25-record`](benchmark-data/polling-data-loop-pmu-2026-08-25-record);
- [`benchmark-data/polling-activation-pmu-2026-08-25-record`](benchmark-data/polling-activation-pmu-2026-08-25-record); and
- [`benchmark-data/polling-data-loop-cycles-2026-08-25-record`](benchmark-data/polling-data-loop-cycles-2026-08-25-record).

Each record contains its exact commands, environment, source diff, summaries
or reports, system snapshots, and SHA-256 manifest.
