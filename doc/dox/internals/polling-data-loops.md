# Polling data loops

## Purpose

A polling data loop removes the per-activation eventfd write, kernel wakeup,
eventfd read, and epoll dispatch from a local PipeWire graph path. It is an
opt-in execution policy for a named data loop. It is not a second graph
scheduler and it does not change the dependency rules maintained by
`module-scheduler-v1`.

The repeated path is:

```text
triggering thread                         polling data-loop thread

write cycle state
decrement dependency count
write signal timestamp
CAS NOT_TRIGGERED -> TRIGGERED  ------->  observe TRIGGERED
                                           CAS TRIGGERED -> AWAKE
                                           process node and peers
                                           CAS AWAKE -> FINISHED
```

There is no eventfd operation or timeout-zero kernel poll in this path.

## Configuration and ownership

Set `loop.idle = busy-spin` on a context data loop. Assign nodes to that loop
with the existing `node.loop.name` or `node.loop.class` properties.

```ini
context.data-loops = [
    {
        loop.name = rtc-input
        thread.name = rtc-input
        loop.class = data.rt
        loop.idle = busy-spin
        loop.rt-prio = -1
        thread.affinity = [ 2 ]
    }
    {
        loop.name = observer
        thread.name = observer
        loop.class = data.rt
        loop.idle = eventfd
        loop.rt-prio = -1
        thread.affinity = [ 4 ]
    }
]
```

For the bounded queue module, place the producer graph and queue input on
`rtc-input`. Place the queue output and telemetry or GUI observer on
`observer`. The queue is the asynchronous graph boundary; merely placing two
nodes from one synchronous graph cycle on different threads does not create
pipeline overlap.

Each prepared local node has exactly one owning data loop. A polling loop scans
only its own bounded list of prepared nodes. Two polling threads never scan the
same list or process the same node.

Use different physical cores, not two SMT siblings, for two latency-critical
polling loops. A telemetry or GUI loop normally remains eventfd- or
clock-driven because the queue already prevents it from delaying the producer.

## Supported sources

A busy-spin data loop supports:

- local graph activation records;
- loop control invocations; and
- transitional `SPA_NODE_FLAG_RTC_PROCESS` nodes that return a bounded work
  count from `spa_node_process()`.

It does not poll arbitrary file descriptors, timers, signals, or idle eventfd
sources. Registering such a source on a busy-spin loop fails with `-ENOTSUP`
instead of silently starving the source. Keep fd-driven nodes on an eventfd
loop, or adapt their completion path to publish a graph activation without a
file-descriptor wakeup.

Control invocations are flushed by the polling thread during its lock handoff
without calling epoll. The loop releases and reacquires its administrative
lock once per bounded scan. When an administrative caller is actually waiting and the
polling thread uses `SCHED_FIFO`, the loop performs a one-nanosecond sleep so a
lower-policy control thread can run. That syscall is confined to lifecycle or
control contention; it is not executed on the uncontended repeated path.

## Activation publication

For a version-1 local activation target:

1. The graph driver prepares the cycle state and publishes
   `NOT_TRIGGERED`.
2. Each dependency atomically decrements `pending`.
3. The final dependency writes `signal_time` and uses a successful atomic CAS
   to publish `TRIGGERED`.
4. The polling owner uses an atomic CAS from `TRIGGERED` to `AWAKE` before it
   reads cycle state or calls the node.
5. The polling owner publishes `FINISHED` after node processing and peer
   activation.

The successful status CAS is the publication boundary. `signal_time` is
written before the publishing CAS so the poller cannot observe a new status
with the preceding cycle's timestamp. The existing atomic dependency counter
continues to prevent a node from running before all required inputs signal it.

## RTC data-loop migration

`pw_rtc_data_loop` remains available as a compatibility fallback.

When an `SPA_NODE_FLAG_RTC_PROCESS` node is assigned to a busy-spin context
data loop, `impl-node` registers that node with the selected polling loop and
does not create a private `pw_rtc_data_loop`. Lifecycle commands add and remove
the node while holding the data-loop lock, so Pause, Suspend, and destruction
cannot overlap its process call. Several RTC nodes may share a named polling
loop; separate `node.loop.name` values give them separate threads.

When the same node is assigned to an eventfd data loop, the existing private
`pw_rtc_data_loop` path remains unchanged. Removing that fallback requires all
remaining RTC-process plugins and deployments to select context-owned polling
loops, and requires complete-buffer graph sources to replace any remaining
latest-buffer-specific execution contracts.

Thus the current change replaces `pw_rtc_data_loop` for configured polling
deployments, but does not yet remove its public API or compatibility tests.

## Measured activation cost

The integration benchmark invokes one ordinary local node in a closed loop.
It records from activation publication immediately before the trigger CAS to
entry into `spa_node_process()`. It uses 1,000 warmup cycles followed by 10,000
measured cycles. These results are development evidence, not deployment
qualification.

Test host on 2026-08-24:

- AMD Ryzen 7 6800H, 8 cores and 16 hardware threads;
- Linux 6.12.57 with `PREEMPT_DYNAMIC`;
- GCC 14.2.0 release build;
- shared, unpinned cores with frequency scaling enabled; and
- closed-loop arrivals.

Five runs produced:

| Idle policy | Rate | p50 | p99 | p99.9 | Per-run maximum range |
| --- | ---: | ---: | ---: | ---: | ---: |
| `eventfd` | 307.3-318.1 kcycles/s | 2.986-3.085 us | 4.138-5.189 us | 7.083-9.548 us | 8.556-28.192 us |
| `busy-spin` | 3.79-4.25 Mcycles/s | 0.140-0.160 us | 0.201-0.220 us | 0.240-0.260 us | 0.271-8.085 us |

Run the benchmark with:

```sh
./build/src/tests/pw-test-rtc-data-loop \
    --benchmark-activation eventfd 10000
./build/src/tests/pw-test-rtc-data-loop \
    --benchmark-activation busy-spin 10000
```

A syscall-count comparison with 100 and 10,000 measured polling cycles found
the same setup-only counts in both runs: five `eventfd2` calls, three writes,
and twenty-three reads. The counts did not scale with polling activations. The
eventfd case added one write, one read, and one epoll wait per warmup or
measured activation.

Deployment qualification must additionally pin distinct physical cores,
control IRQ placement and frequency policy, use fixed-rate open-loop arrivals,
exercise overload, and report p99.9 and maximum latency together with graph
completion, queue age, drops, CPU consumption, and lifecycle interference.
