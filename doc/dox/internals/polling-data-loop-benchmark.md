\page page_polling_data_loop_benchmark Polling data-loop benchmark

# Polling data-loop benchmark

## Scope

This benchmark isolates two scheduler costs:

1. wake-to-process latency for the normal eventfd and busy-spin activation
   paths; and
2. the time between polling scans while the loop performs its mandatory
   control-queue flush and lock handoff.

It is a diagnostic experiment, not a complete RTC acceptance test. The
activation experiment is closed-loop with one activation in flight and an
otherwise empty process callback. It does not model camera SDK work, algorithm
execution, buffer pressure, burst arrivals, or end-to-end graph latency.
CPU counter and call-graph results are documented separately in
[Polling data-loop profiling](polling-data-loop-profiling.md).

## Reproduction

Build `pw-test-polling-data-loop`, then run:

```console
python3 src/tests/benchmark-polling-data-loop.py \
    build/src/tests/pw-test-polling-data-loop \
    polling-results \
    --rt-priority 0 \
    --rt-priority 88
```

The runner selects two different physical cores in one CPU package unless
explicit CPU numbers are supplied. It rejects a run when the loop executes on
the wrong CPU, when the requested real-time policy is not active, when a
histogram has the wrong sample count, or when a control invocation fails.

Each campaign records:

- five randomized repetitions of every cell;
- 170 measured cells and 152,000,000 timed observations in total;
- 10,000 warmup and 100,000 measured activation cycles per wake policy;
- 100,000 warmup and 1,000,000 measured polling scans per scan cell;
- polling source counts of 0, 1, 4, 16, and 64, in addition to the timing
  probe;
- no contention, one asynchronous control invocation every 50 microseconds,
  and bursts of 16 invocations every millisecond;
- `SCHED_OTHER` and `SCHED_FIFO` priority 88 loop policies;
- exact run-length histograms of integer nanoseconds, without reservoir
  sampling;
- result storage pre-faulted before warmup so first-write minor faults do not
  contaminate the measured latency distribution;
- individual summaries, aggregates, commands, the executable hash, source
  diff, build options, CPU topology and frequency policy, kernel configuration,
  and before/after system counters; and
- SHA-256 hashes for every recorded artifact.

The activation clock is the PipeWire SPA system monotonic clock. Polling scan
gaps use `CLOCK_MONOTONIC_RAW`. Percentiles use nearest-rank selection. The
aggregate table reports the median of each statistic across five repetitions,
along with its repetition range.

## Recorded campaign

The 2026-08-25 campaign ran an optimized-with-debug-information build on an AMD
Ryzen 7 6800H under Linux 6.12.57 with `PREEMPT_DYNAMIC` configured for full
preemption. Caller CPU 12 and loop CPU 14 are different physical cores. The
cores were not isolated, SMT remained enabled, the `amd-pstate-epp` governor
was `powersave`, and boost remained enabled. These facts make the results
representative of this development host, not a hardware-independent bound.

The complete record is in
[`benchmark-data/polling-data-loop-2026-08-25-record`](benchmark-data/polling-data-loop-2026-08-25-record).
Start with `metadata.json`, `environment.txt`, `summary.csv`, and
`aggregate.csv`; the per-cell CSV files contain the exact histograms.

### Activation latency

Values below are the median statistic across five repetitions. Maximum is the
median of the five per-repetition maxima, not the maximum across all runs.

| Loop policy | Scheduler | p50 | p99 | p99.9 | Median maximum | Cycles/s |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| busy-spin | `SCHED_OTHER` | 0.230 us | 0.321 us | 1.052 us | 11.431 us | 2,992,011 |
| eventfd | `SCHED_OTHER` | 4.319 us | 5.961 us | 8.586 us | 302.836 us | 224,075 |
| busy-spin | `SCHED_FIFO(88)` | 0.230 us | 0.321 us | 0.981 us | 13.585 us | 3,029,595 |
| eventfd | `SCHED_FIFO(88)` | 2.935 us | 4.378 us | 6.913 us | 18.504 us | 326,395 |

On this host, busy-spin reduced median wake-to-process latency by 18.8 times
under `SCHED_OTHER` and 12.8 times under `SCHED_FIFO(88)`. At p99.9 the
reductions were 8.2 and 7.0 times, respectively. The result supports keeping
busy-spin as an opt-in latency policy. It does not justify making it the
default because the polling thread consumes its assigned CPU continuously.

### Empty polling scans

The table shows cells without injected control traffic. `Sources` counts
no-op scheduled sources; the timing probe is additional.

| Scheduler | Sources | Median p50 gap | Median p99 gap | Median p99.9 gap | Median scans/s |
| --- | ---: | ---: | ---: | ---: | ---: |
| `SCHED_OTHER` | 0 | 0.040 us | 0.051 us | 0.061 us | 24,191,646 |
| `SCHED_OTHER` | 1 | 0.040 us | 0.051 us | 0.070 us | 23,827,134 |
| `SCHED_OTHER` | 4 | 0.041 us | 0.070 us | 0.080 us | 22,116,027 |
| `SCHED_OTHER` | 16 | 0.060 us | 0.071 us | 0.091 us | 16,338,255 |
| `SCHED_OTHER` | 64 | 0.150 us | 0.180 us | 0.220 us | 6,573,116 |
| `SCHED_FIFO(88)` | 0 | 0.040 us | 0.051 us | 0.060 us | 24,314,933 |
| `SCHED_FIFO(88)` | 1 | 0.040 us | 0.051 us | 0.070 us | 23,749,468 |
| `SCHED_FIFO(88)` | 4 | 0.041 us | 0.051 us | 0.071 us | 22,225,788 |
| `SCHED_FIFO(88)` | 16 | 0.060 us | 0.071 us | 0.100 us | 16,337,496 |
| `SCHED_FIFO(88)` | 64 | 0.150 us | 0.160 us | 0.210 us | 6,682,226 |

The ordinary scan cost scales gradually with source count. At 64 no-op
sources, the median complete-scan gap remained 150 nanoseconds. The p99.9 gap
was 60 nanoseconds with no work sources and 210 to 220 nanoseconds with 64.

Periodic and burst control traffic completed every submitted invocation with
zero failures. It did not change the 64-source `SCHED_FIFO(88)` p50, while the
p99.9 medians increased from 0.210 microseconds with no contention to 0.321
with periodic contention and 0.381 with burst contention. This evidence does
not support removing the per-scan `spa_loop_control_yield()` fairness point.

Per-repetition maxima remained noisy. The largest median of five scan maxima
was 16.841 microseconds, and the largest individual scan maximum was 43.952
microseconds. Because the host CPUs were not isolated, these maxima cannot be
attributed to the PipeWire mutex or control queue alone. A deployment latency
claim still requires a longer
end-to-end campaign on the target kernel, isolated CPU layout, camera and AO
hardware, production real-time configuration, and defined p99.9/max gates.

## Interpretation constraints

- The closed-loop activation experiment intentionally isolates one wake. It
  must not be used as an overload or throughput-service-level claim.
- The scan histogram includes the timing call, remaining source probes,
  polling relaxation, queue flush, unlock, and relock. It does not separately
  attribute those instructions.
- Dynamic frequency scaling, boost, SMT siblings, interrupts, and unrelated
  host activity remained possible and are captured as limitations rather than
  silently controlled away.
- Exact maxima are reported, but a five-repetition development-host campaign
  is not long enough to establish a worst-case execution-time bound.
