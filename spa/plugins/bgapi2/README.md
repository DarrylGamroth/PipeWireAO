# BGAPI2 SPA plugin

This optional plugin is the native PipeWireAO integration for GenICam
producers supported by Baumer GAPI. Enable it with `-Dbgapi2=enabled`; use
`-Dbgapi2-prefix=PATH` when the SDK is not installed under
`/opt/baumer-gapi-sdk-c`. A disabled build does not inspect or include the
proprietary SDK.

## Implemented slice

The `api.bgapi2.source` factory provides complete-frame capture and an opt-in
progressive profile with:

- an explicit GenTL producer path and optional interface, device, and stream
  indices;
- standard SPA node, port, format, buffer, metadata, I/O, and command methods;
- immutable `SPA_NODE_FLAG_RTC_PROCESS` ownership;
- `SPA_IO_BuffersLatestLink` fan-out without graph-ready callbacks;
- mapped `MemPtr` or `MemFd` buffers announced directly to BGAPI2, with no
  image copy;
- `Mono8` and unpacked `Mono10`, `Mono12`, `Mono14`, and `Mono16` formats;
- dynamic GenICam Boolean, Integer, Float, Enumeration, and String controls
  through `SPA_PARAM_PropInfo` and `SPA_PARAM_Props`;
- fixed `SPA_META_Header` and initialized Version 1 `SPA_META_Acquisition`
  metadata;
- first-class `SPA_META_Progressive` publication over mapped host memory; and
- synchronous acquisition stop and event-thread shutdown before pool teardown.

The required factory property is:

```text
api.bgapi2.producer=/absolute/path/to/producer.cti
```

The optional `api.bgapi2.interface-index`, `api.bgapi2.device-index`, and
`api.bgapi2.stream-index` properties select a device. By default the source
searches all interfaces and selects device and stream zero. The actual indices,
model, serial number, and producer path are published as node properties.

`api.bgapi2.progressive=disabled|offer|require` is also fixed at construction;
the default is `disabled`. `offer` negotiates progressive metadata when the
consumer supplies it and otherwise retains complete-frame operation. `require`
rejects a pool without the metadata and rejects a producer that completes a
frame without exposing at least one intermediate committed prefix. This keeps a
producer that merely reports the final byte count from satisfying the physical
progressive contract.

The plugin discovers scalar controls from the remote GenICam NodeMap instead of
maintaining a camera-specific list. Properties use canonical names such as
`genicam.ExposureTime` and `genicam.PixelFormat`; enumeration labels, numeric
ranges, descriptions, and current access state come from the camera. A write
contains exactly one typed property and is accepted only while acquisition is
stopped. Layout-changing controls also require all buffers to be released and
invalidate format and buffer negotiation. GenICam command nodes are not exposed
as persistent SPA properties.

The plugin uses `spa_image_source`, `spa_image_source_latest`, and
`spa_buffer_latest` directly. It has no `pw_stream`, libpipewire client,
private image pool, payload copy, or graph scheduling path.

The SPA node and transport adapter are C. `camera.cpp` is a narrow C++
containment boundary because BGAPI2 can propagate C++ GenApi exceptions through
its nominal C API. Every vendor call is caught before it can unwind through C;
the adapter otherwise exposes a C interface and does not use BGAPI2's C++ object
model.

## Completion ownership

The default `api.bgapi2.completion-mode=callback` profile uses BGAPI2's
new-buffer event handler. In complete mode the vendor event thread reads the
completed buffer's owner and dynamic frame metadata, then publishes one
fixed-size completion descriptor through SPA's cache-line-isolated SPSC ring.
The PipeWireAO RTC data loop is the sole consumer and remains responsible for
validating and publishing the frame.

Progressive mode narrows the callback handoff further: the callback publishes
only the completed BGAPI2 buffer identity. After acquiring that SPSC entry, the
RTC loop reads all terminal metadata itself. The same RTC loop owns ongoing
filled-size queries, progressive metadata updates, terminal publication, and
buffer requeue. Consequently no vendor callback and RTC operation query the same
buffer concurrently. Callback scheduling can delay terminal completion, but it
does not delay a prefix that the RTC loop has already observed and published.

The construction-time `api.bgapi2.completion-mode=polling` profile instead
calls `BGAPI2_DataStream_GetFilledBuffer(stream, ..., 0)` from the RTC process
function. It is provided for controlled latency comparisons and avoids an
event-thread handoff. It is not the default because both tested producers build
error details on empty polls; Euresys in particular allocates on this path.
Completion mode cannot change while the node is alive.

The ring contains at most the 64 buffers allowed by `spa_image_source`; overflow
is a fatal acquisition error, not a lossy overwrite. An overflow would mean the
vendor delivered more unique completions than the announced pool can contain.
The event handler is installed at Start and synchronously removed after
acquisition stops on Pause or Suspend. Restart creates an empty completion
queue before the handler is enabled again.

Returning a subscriber lease still calls `BGAPI2_DataStream_QueueBuffer` from
the RTC owner. This preserves direct buffer ownership and avoids another bridge
thread, but the GenTL producer's queue implementation is part of the real-time
contract.

## Progressive ownership

Progressive operation is limited to directly mapped `MemPtr` or `MemFd` host
memory and an exact unpacked image layout. The payload must equal width times
bytes per pixel times height, and one complete image row is the publication
granularity. DMA-BUF progressive operation is deliberately not offered.

After each queue operation the RTC owner records the buffer's filled-size
baseline. With no active progressive buffer, each process duty probes at most
one eligible pool slot. A change from that baseline identifies a new fill epoch;
the RTC loop then publishes the buffer as Active and polls only that buffer until
the terminal callback arrives. Prefixes are rounded down to whole rows and must
remain within the negotiated payload. Stop, layout failure, incomplete delivery,
and protocol failure all produce an explicit terminal Aborted state.

The implementation does not repeatedly scan `GetIsAcquiring`. Heaptrack showed
that Euresys Gigelink allocates twice inside `DSGetBufferInfo` for every such
query. `GetSizeFilled` had no attributed per-query allocation with either tested
producer. The one-query-per-duty policy makes the work bound independent of the
number of eligible buffers in that duty; round-robin selection provides eventual
observation across the small fixed pool.

This uses the same BGAPI2 buffer-information mechanism as HEART's CoaXPress
streaming WFS path, but changes its ownership and work bound: HEART scans for an
acquiring buffer and then polls its filled size, whereas this plugin uses the
queued-size baseline and at most one probe per RTC duty. HEART's source also
records that BGAPI2 2.15.2 returned zero from `GetSizeFilled` where an earlier
2.12.2/Euresys combination worked. The installed 2.16.1 headers and runtime
provide the query, but physical Coaxlink behavior remains an explicit
qualification item rather than an inferred capability.

## Timing and metadata

`SPA_META_Header.seq` uses the GenTL frame ID and marks a discontinuity when the
sequence is not consecutive. `SPA_META_Header.pts` is the local
`CLOCK_MONOTONIC` time immediately before publication. Acquisition metadata is
valid and initialized, but this version does not claim an exposure-start time,
hardware acquisition identity, clock mapping, or uncertainty.

The local `CLOCK_MONOTONIC` timestamp used when progressive publication begins
is a publication observation, not an exposure timestamp. The source still does
not claim hardware acquisition identity, clock mapping, or uncertainty.

## Qualification

The factory test verifies load and parameter validation without opening a
camera. Camera and source tests use four or eight external host buffers,
respectively. The source test captures ten frames, returns every subscriber
lease, pauses and restarts halfway through the run, and performs ordered
teardown.

On 2026-08-23 the factory, camera, callback-source, and polling-source tests
passed against the connected 640x480 Mono8 GE34GM camera with the BGAPI2
2.16.1 runtime through both:

```text
/opt/euresys/egrabber/lib/x86_64/gigelink.cti
/opt/baumer-gapi-sdk-c/lib/libbgapi2_gige.cti
```

The second result is significant: the Baumer producer does operate this camera.
An earlier failure was caused by an uncaught GenApi exception from the Euresys
producer, which terminated the shared process before the Baumer case ran.

The connected GigE camera also completed ten frames in `progressive=offer`
through both the Euresys and Baumer producers:

```text
captured 10 frames: progressive=10 partial-prefix=0
```

This validates progressive negotiation, Active-to-Complete lifecycle,
subscriber release, restart, and terminal metadata propagation. It does not
qualify physical progressive DMA: no intermediate prefix was observable on the
Gigelink path. The `require` profile rejects this result with `ENOTSUP`, as
intended. A Grablink or Coaxlink system is still required to validate filled-size
reset, monotonic partial rows, DMA visibility, cancellation, restart, and tail
latency while acquisition is active.

The separately installed BGAPI2 2.16.1 C and C++ packages contain byte-identical
`libbgapi2_genicam`, `libbgapi2_img`, and Baumer GigE CTI binaries. Both
`BGAPI2_DataStream_GetFilledBuffer` and the C++
`DataStreamEventControl::GetFilledBuffer` ultimately call the same internal
`CDataStreamObj::getFilledBuffer`; the C entry point is a direct checked tail
call, while the C++ entry point adds object guards, RTTI, and exception
translation. The C++ package therefore does not provide a different producer or
completion engine, and changing facades would not address producer allocations
or lifecycle behavior.

A ten-frame closed-loop `heaptrack` experiment compared three completion
strategies:

- timeout-zero `GetFilledBuffer` constructed producer error strings on every
  empty poll;
- `GetNumAwaitDelivery` correctly identified the output queue, but Euresys
  allocated twice in `DSGetInfo` per query, while Baumer did not; and
- the event-handler/SPSC implementation made no allocation under
  `bgapi2_camera_try_get_completion`, made no polling `GetFilledBuffer` or
  `GetNumAwaitDelivery` calls, and made no allocation in the SPSC callback
  handoff itself.

Euresys still allocated while the callback queried metadata for each actual
frame. Those calls now run on the vendor event thread. Both producers made an
allocation-bearing `DSQueueBuffer` call for each of the nine returned leases.
The source is therefore qualified for allocation-free empty RTC polling, but it
is not qualified for a strict zero-allocation BusySpin process path.

The experiment used a debug-optimized build without LTO. Whole-process totals
include SDK loading, camera discovery, XML parsing, and test setup, so they are
not frame-latency measurements. Tail latency and scheduling jitter still require
an open-loop, timestamped hardware run with CPU affinity, warmup, histograms,
and causal scheduler counters.

The completion microbenchmark removes setup and delivered frames from the
reported empty-dequeue samples. It first waits for and returns one frame, then
records 200,000 calls with `CLOCK_MONOTONIC_RAW` around each operation. The
back-to-back clock-pair distribution is reported beside the operation rather
than subtracted. Exact samples are sorted after acquisition stops; this build
does not require a C HdrHistogram dependency.

Three runs per producer and profile on CPU 15 with BGAPI2 2.16.1 produced:

| Producer | Profile | p50 | p99 | p99.9 |
| --- | --- | ---: | ---: | ---: |
| Euresys Gigelink | callback | 20 ns | 31 ns | 31 ns |
| Euresys Gigelink | polling | 2.41–2.49 us | 2.54–4.27 us | 10.0–11.4 us |
| Baumer GigE | callback | 20 ns | 31 ns | 31 ns |
| Baumer GigE | polling | 200–211 ns | 211–251 ns | 360–410 ns |

The clock-pair baseline was 20 ns p50 and 30 ns p99, so the callback result is
at this harness's measurement floor. These are not production qualification
numbers: the host was an AMD Ryzen 7 6800H running Linux 6.12.57 with
`preempt=full`, the `powersave` governor, SMT enabled, and no isolated core.
They do establish that callback empty-dequeue is negligible compared with
timeout-zero BGAPI2 polling on both producers.

The same harness measured the two buffer-information queries used to evaluate
progressive polling. Three 200,000-query runs produced:

| Producer | Query | p50 | p99 | p99.9 |
| --- | --- | ---: | ---: | ---: |
| Euresys Gigelink | `GetIsAcquiring` | 220 ns | 230–240 ns | 280–341 ns |
| Euresys Gigelink | `GetSizeFilled` | 30 ns | 31 ns | 40–50 ns |
| Baumer GigE | `GetIsAcquiring` | 60 ns | 61 ns | 80–110 ns |
| Baumer GigE | `GetSizeFilled` | 30 ns | 31 ns | 40–41 ns |

The clock-pair baseline was 20 ns p50 and 30 ns p99. Heaptrack over 10,000
queries attributed exactly 20,000 allocations to Euresys
`GetIsAcquiring`, inside its `DSGetBufferInfo`, and none to the other three
producer/query combinations. These measurements motivate the filled-size
baseline algorithm; they do not qualify a Coaxlink data path that is not present
on this host.

Heaptrack attributes no allocation to callback-mode
`bgapi2_camera_try_get_completion` for either producer. Polling calls
`GetLastTLError` on every empty dequeue. Euresys reaches `GCGetLastError`; Baumer
reaches its CTI `GetLastError`; both construct `std::string` objects and allocate.
Under heaptrack the Baumer polling source made millions of allocations and
could not deliver ten frames before the three-second test deadline. Its faster
unprofiled polling time therefore does not make it a strict RTC path.

Repeated-process testing also found a Baumer CTI lifecycle nonconformance. After
one Baumer-only camera test closes cleanly, the next Baumer-only process opens
and starts the camera but receives no buffer. Every adapter stop, discard,
revoke, stream close, device close, interface close, system close, and release
call reports success. One Euresys Gigelink capture restores stream delivery, and
the next Baumer run then succeeds. The failure remains reproducible with the
2.16.1 runtime and Baumer CTI. This behavior is recorded as producer evidence;
the plugin does not add a vendor-specific stream-reset workaround.

The profiling commands were:

```console
heaptrack -o /tmp/bgapi2-euresys \
  build/spa/plugins/bgapi2/spa-bgapi2-capture-test \
  build/spa/plugins/bgapi2/libspa-bgapi2.so \
  /opt/euresys/egrabber/lib/x86_64/gigelink.cti
heaptrack_print /tmp/bgapi2-euresys.zst \
  -F /tmp/bgapi2-euresys.stacks --flamegraph-cost-type allocations
```

Repeat the same commands with the Baumer CTI to compare producers.

Run the operation benchmark with:

```console
taskset -c 15 build/spa/plugins/bgapi2/spa-bgapi2-completion-benchmark \
  /opt/euresys/egrabber/lib/x86_64/gigelink.cti callback 200000
taskset -c 15 build/spa/plugins/bgapi2/spa-bgapi2-completion-benchmark \
  /opt/euresys/egrabber/lib/x86_64/gigelink.cti polling 200000
taskset -c 15 build/spa/plugins/bgapi2/spa-bgapi2-completion-benchmark \
  /opt/euresys/egrabber/lib/x86_64/gigelink.cti size-filled 200000
```

## Remaining work

- Add manager and device factories for live camera discovery and reconciliation.
- Map a hardware or producer timestamp into the PipeWireAO acquisition clock
  contract before claiming exposure timing.
- Qualify `progressive=require` on physical Grablink or Coaxlink hardware,
  including monotonic partial-row visibility, cancellation, restart, and
  allocation and latency tails.
- Extend pixel-format coverage where a deterministic direct SPA mapping exists.
- Run open-loop latency and tail-jitter qualification at the intended camera
  rates and scheduling profiles.
- Resolve or formally exclude the Baumer CTI repeated-process stream failure
  before claiming that producer for unattended lifecycle operation.
- Decide whether a dedicated SDK-owning requeue agent is justified for strict
  zero-allocation operation. It should only be added with evidence that its
  extra handoff and scheduling cost improves the end-to-end deadline result.
