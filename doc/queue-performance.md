# Bounded queue ring verification

The queue ring transfers generation-tagged tokens between one capture
producer and one playback consumer. The completion ring reverses those
roles. This record covers the C ring operations used by those callbacks,
and measures a separate diagnostic on the complete producer graph cycle.
Production buffer ownership, payload copying and leases remain described
in [the queue contract](../docs/queue.md).

2026-11-25 comparison. Baseline queue source
`pipewireao-spa-plugins-core@e227bd920658ae192187b4609e26d641c1f5c123`.
Native import `6285e539a`. Candidate `47ba74dda`.

## Ownership and progress

| Ring state | Writer | Publication and observation |
| --- | --- | --- |
| Capacity, slot mask and policy | Initializing thread | Immutable while either endpoint uses the ring. |
| Write position | Producer | Release store publishes the relaxed slot store and preceding payload writes. A consumer acquires the write position before reading a slot. |
| Read position with backpressure or drop-newest | Consumer | Release store follows the slot read. Producer acquires the read position before reusing the slot. |
| Read position with drop-oldest | Producer and consumer | Both arbitrate ownership with a strong acquire-release compare-exchange. The winner owns the removed token. |
| Slot value | Producer | Atomic relaxed access permits a losing consumer to speculatively read a slot that the producer is reusing. Peek conveys no ownership. |

A consumer acquires the read position before loading the write position. An
eviction's release publishes the producer's earlier write positions. This
prevents a consumer from comparing a newly evicted read position with a stale
write position behind it. A successful claim observes the write publication and
still owns the read position it inspected. If the producer loses its eviction
compare-exchange, the consumer has made room. The producer acquires the
consumer's release through that failed compare-exchange before publishing into
the freed slot. This orders the consumer's candidate load before the overwrite
even when the previous head and the new tail map to the same physical slot.
The caller quiesces both endpoints before resetting the positions.

Both positions are 64-bit unsigned. The physical slot cycle is the next power
of two at or above the configured capacity. Masking remains valid across wrap
because the slot cycle divides the position cycle. The capacity check still uses
the exact configured depth. The module's 58-bit token sequence runs out before
either position could make a full lap while an operation holds an old snapshot.

The allocated module follows the alignment of its C type. The ring
configuration, read position, write position and slot array start on separate
`SPA_CACHE_LINE_SIZE` boundaries. The positions and slot array are at offsets
64, 128 and 192 bytes on x86-64 and 128, 256 and 384 bytes on AArch64. A
drop-oldest race shares the read position by design. Both endpoints also access
published slots. The change adds no line of padding and preserves the existing
slot array size.

The ring allocates no storage, acquires no mutex and issues no system call
during push, admit, peek, pop or claim. The build requires always-lock-free
64-bit C atomics. With a single read-position writer, pop and claim finish
after one snapshot and use a release store. Drop-oldest admission attempts one
compare-exchange and pop or claim attempts at most two. A losing consumer can
return 0 or `-EAGAIN`, according to the existing operation contract. AArch64's
baseline LL/SC instruction sequence may retry inside one strong compare-exchange,
so the C-level attempt limits do not establish wait-freedom on that target.
These progress bounds apply to the ring functions. They do not establish a
progress guarantee or a deadline for PipeWire callbacks or scheduling.

## Correctness and machine code

- At depth three, starting two publications before the position rolls over,
  the baseline accepted 10, 11 and 12 and returned 10, then 12 in place of 11.
  The candidate passed the same FIFO sequence at its 64-bit wrap boundary.
- The state selector's replacement stress admits 200,000 tokens per depth at
  depths 1, 17 and 64. It checks the consumer's publication order and payload
  visibility, then checks that each token was consumed or displaced exactly
  once. The selector passed optimized x86-64, ASan with UBSan, and TSan builds.
- Clang 19.1.7 cross-produced AArch64 state selectors with both
  `-march=armv8-a -mno-outline-atomics` and
  `-march=armv8.1-a+lse -mno-outline-atomics`. Both passed under QEMU user-mode
  10.0.13 using `cortex-a53` and `max` respectively. This is software execution
  evidence. A physical AArch64 CPU was unavailable for timing or race stress.
- GCC 14.2 x86-64 disassembly showed no integer division in the candidate
  ring and `lock cmpxchg` only on the drop-oldest paths. AArch64 emits
  `ldar` and `stlr` for the acquire and release positions. The competing
  paths emit `ldaxr`/`stlxr` on Armv8-A and `casal` with LSE.
- The live fixture passed both storage cases for every comparison run. The
  fixture checks requested sequence publication, including recovery while
  the observer holds its initial buffer.

## Performance

The [ring harness](../src/modules/queue/tests/benchmark-ring.c) drives one
synthetic token producer and consumer on logical CPUs 10 and 12, separate
physical cores on one AMD Ryzen 7 6800H socket and NUMA node. Each invocation
warms with 32,768 tokens, resets, and drives 1,048,576 publications. The
producer retries rejected admissions in the backpressure case and drives
new arrivals as fast as it can in the drop-oldest case. The consumer drains
either with pop or with peek and conditional claim. The harness labels those
paths `copy` and `lease` to correspond to the playback callback branches.
No payload copies, stream scheduling or cadence are measured in this harness.

The benchmark samples every 64th endpoint attempt with
`CLOCK_MONOTONIC_RAW` and keeps the result check and sample sort outside the
timed call. Samples include empty and full attempts and the clock overhead
(about 20 ns for two reads). Wall time includes retries and draining. The
timed steady-state ring calls allocate zero bytes. The environment was
Debian with Linux 6.12.117, GCC 14.2.0 `-O3 -DNDEBUG` and the `powersave`
governor. Baseline and candidate ran alternately for five repetitions each.
The lower-background-load ring comparison used byte-identical x86-64 candidate
`.text` to the final executable, verified after strengthening the C CAS
failure order. A second comparison with the final source ran during concurrent
Rust and Julia builds and is recorded separately.

Five-run medians and wall ranges from the lower-background-load ring comparison
follow. Latency entries give the median across runs of each consumer attempt
percentile, p50 / p99 / p99.9 in ns. User cycles are whole-process `perf stat` cycles per
publication, including warmup and the harness. At these short durations the
counters were multiplexed. Percentiles are per-run observations, not deadlines.

| Overflow and consumer | Depth | Baseline → candidate wall ms [range] | Consumer ns baseline → candidate | User cycles per publication | Drop rate |
| --- | ---: | --- | --- | --- | --- |
| Backpressure, pop | 1 | 112.46 [107.78–122.35] → 112.08 [94.25–123.93] | 80 / 130 / 150 → 50 / 131 / 141 | 961 → 959 | 0% |
| Backpressure, pop | 3 | 88.14 [80.07–91.46] → 46.74 [44.15–49.03] | 90 / 131 / 141 → 50 / 120 / 141 | 729 → 397 | 0% |
| Backpressure, pop | 64 | 86.06 [83.06–91.28] → 24.08 [22.11–25.18] | 100 / 131 / 150 → 60 / 120 / 141 | 708 → 205 | 0% |
| Backpressure, peek and claim | 3 | 104.54 [94.27–105.46] → 53.32 [50.25–58.16] | 120 / 190 / 230 → 60 / 150 / 180 | 845 → 456 | 0% |
| Drop-oldest, pop | 1 | 82.40 [78.52–82.93] → 89.83 [84.13–91.24] | 60 / 231 / 260 → 60 / 210 / 250 | 657 → 718 | 90.14% → 79.47% |
| Drop-oldest, pop | 3 | 69.47 [67.22–71.43] → 60.63 [15.81–63.32] | 120 / 240 / 261 → 130 / 251 / 290 | 557 → 481 | 48.46% → 41.03% |
| Drop-oldest, peek and claim | 3 | 77.38 [73.58–84.49] → 65.74 [15.76–78.22] | 101 / 210 / 230 → 120 / 230 / 270 | 611 → 523 | 67.55% → 57.15% |

The depth-three and depth-64 backpressure cases transfer more tokens per second
with less total CPU use. The depth-one backpressure result overlaps in both
directions. At saturation, changes in the drop rate change how much work
the consumer completes. In particular, the depth-one drop-oldest producer
takes longer while the consumer completes more publications. The drop-oldest
runs establish no general performance improvement.

User-cycle sampling with `perf record` isolated the ring in the depth-three
backpressure peek-and-claim workload. The first comparison's standalone
profiles attributed about 75% of samples to admit, peek and claim on each
version. The claim symbol fell from 23.1% to 9.5%; the instructions for that
path changed from compare-exchange to a release store. The profiles also
sampled harness work. Neither sampling nor the shared-cache miss counters
attribute performance changes to a particular cache-line transfer. The final
busy comparison still showed depth-three backpressure pop at
191.07 → 109.53 ms, peek and claim at 213.70 → 111.79 ms, and depth-64 pop at
188.92 → 51.99 ms.

The live producer-cycle fixture uses a capacity-one drop-oldest queue with
the observer retaining sequence 1 and a 64-byte ndarray payload. It warms
1,000 requests and times 10,000 per mode, from
`pw_stream_trigger_process()` to the RT `trigger_done` callback. PipeWireAO
SDK 4.0.8 supplied the scheduler and runtime modules for both builds. The lower-load candidate graph run used the parser before its
cold source move in `ec44957f8`. The higher-load run used the final source. The
two-CPU mask confines the fixture process and its threads. The graph threads
can move within that mask. The fixture drives no downstream graph cycles
during timing, so it measures no copy or lease delivery. Scheduling and
wake-up dominate the result.

| Background load | Storage | Baseline → candidate median rate, requests/s | Median p50 / p99 / p99.9, µs |
| --- | --- | --- | --- |
| Lower | Copy | 122,340 → 124,503 | 7.78 / 10.63 / 19.59 → 7.75 / 9.96 / 21.07 |
| Lower | Lease | 83,333 → 84,475 | 11.52 / 19.82 / 28.57 → 11.79 / 16.65 / 30.04 |
| Higher | Copy | 47,962 → 44,645 | 9.02 / 10.78 / 3,737.26 → 8.06 / 11.00 / 3,866.09 |
| Higher | Lease | 34,024 → 38,355 | 18.42 / 23.66 / 4,554.92 → 12.71 / 23.37 / 4,492.25 |

The copy requests used one producer cycle each. Lease averaged 1.5 and
occasionally needed two. The request distributions and rates do not establish a
graph speedup. The fixture emitted its usual missing D-Bus support, missing
initial format and mixer warnings on both builds. No live device, open-loop
arrival schedule, long soak, deadline or hardware result is inferred.

## Reproduction

From the native checkout, set `SDK` to the installed PipeWireAO 4.0.8 prefix
and `BASE` to a checkout of the recorded source baseline. Choose two CPUs on
separate cores. The retained
[per-run results](queue-performance-runs.csv) contain the counters and latency
summaries. The original logs, disassembly and sampled profiles are under
`/var/tmp/pwao-queue-performance.DcUZFanK` on the measurement host. The harness
did not export individual latency samples.

```sh
export TMPDIR=/var/tmp
export PKG_CONFIG_PATH="$SDK/lib/pkgconfig"
out=$(mktemp -d /var/tmp/queue-bench.XXXXXXXX)
q=src/modules/queue
cc -std=c11 -O3 -DNDEBUG -Wall -Wextra -Werror -pthread \
  $(pkg-config --cflags libspa-ao-0.2) -I"$q" \
  "$q/queue.c" "$q/tests/benchmark-ring.c" -o "$out/candidate"
cc -DPWAO_QUEUE_BENCH_BASELINE -std=c11 -O3 -DNDEBUG -Wall -Wextra -Werror -pthread \
  $(pkg-config --cflags libspa-ao-0.2) -I"$BASE/$q" \
  "$BASE/$q/queue.c" "$q/tests/benchmark-ring.c" -o "$out/baseline"

perf stat -e task-clock,cycles,instructions,branches,branch-misses,cache-references,cache-misses \
  -- taskset -c 10,12 "$out/candidate" backpressure lease 3 10 12
perf record -e cycles:u -c 120003 -o "$out/ring.data" \
  -- taskset -c 10,12 "$out/candidate" backpressure lease 3 10 12
perf report --stdio --no-children --sort=symbol -i "$out/ring.data"
```

For the live diagnostic, build the original harness and module with
`meson setup "$out/base" "$BASE" --buildtype=release -Dqueue-only=true -Dtests=enabled`
and `meson compile -C "$out/base" -j 2`. Compile the candidate module against the
same SDK using `-std=c11 -O3 -D_GNU_SOURCE -fPIC -fvisibility=hidden -pthread`,
the candidate's `module-queue.c`, `queue.c`, `buffer-transfer.c` and the
candidate `spa/buffer/meta-json.h`. Put only that additional header in an
include directory beside the build to keep the SDK headers otherwise
identical. Link a shared object with
`pkg-config --cflags --libs libpipewire-ao-0.3`, and define
`PACKAGE_VERSION` to the quoted string `0.1.0`. The baseline module uses the
same flags with the baseline metadata source. Pass each resulting module path
to the same baseline live fixture.

```sh
export LD_LIBRARY_PATH="$SDK/lib"
export PIPEWIREAO_CONFIG_DIR="$SDK/share/pipewire-ao"
export PIPEWIREAO_SPA_PLUGIN_DIR="$SDK/lib/spa-ao-0.2"
export PIPEWIREAO_MODULE_DIR="$SDK/lib/pipewire-ao-0.3"
taskset -c 10,12 "$out/base/$q/pipewireao-queue-live-test" "$queue_module" --benchmark
```
