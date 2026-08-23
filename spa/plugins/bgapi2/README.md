# BGAPI2 SPA plugin

This optional plugin is the native PipeWireAO integration for GenICam
producers supported by Baumer GAPI. Enable it with `-Dbgapi2=enabled`; use
`-Dbgapi2-prefix=PATH` when the SDK is not installed under
`/opt/baumer-gapi-sdk-c`. A disabled build does not inspect or include the
proprietary SDK.

## Implemented slice

The `api.bgapi2.source` factory provides complete-frame capture with:

- an explicit GenTL producer path and optional interface, device, and stream
  indices;
- standard SPA node, port, format, buffer, metadata, I/O, and command methods;
- immutable `SPA_NODE_FLAG_RTC_PROCESS` ownership;
- `SPA_IO_BuffersLatestLink` fan-out without graph-ready callbacks;
- mapped `MemPtr` or `MemFd` buffers announced directly to BGAPI2, with no
  image copy;
- `Mono8` and unpacked `Mono10`, `Mono12`, `Mono14`, and `Mono16` formats;
- fixed `SPA_META_Header` and initialized Version 1 `SPA_META_Acquisition`
  metadata; and
- synchronous acquisition stop and event-thread shutdown before pool teardown.

The required factory property is:

```text
api.bgapi2.producer=/absolute/path/to/producer.cti
```

The optional `api.bgapi2.interface-index`, `api.bgapi2.device-index`, and
`api.bgapi2.stream-index` properties select a device. By default the source
searches all interfaces and selects device and stream zero. The actual indices,
model, serial number, and producer path are published as node properties.

The plugin uses `spa_image_source`, `spa_image_source_latest`, and
`spa_buffer_latest` directly. It has no `pw_stream`, libpipewire client,
private image pool, payload copy, or graph scheduling path.

The SPA node and transport adapter are C. `camera.cpp` is a narrow C++
containment boundary because BGAPI2 can propagate C++ GenApi exceptions through
its nominal C API. Every vendor call is caught before it can unwind through C;
the adapter otherwise exposes a C interface and does not use BGAPI2's C++ object
model.

## Completion ownership

`BGAPI2_DataStream_GetFilledBuffer(stream, ..., 0)` is not a suitable BusySpin
poll. Both tested producers construct error details when its output queue is
empty. The source therefore uses BGAPI2's new-buffer event handler. The vendor
event thread reads the completed buffer's owner and dynamic frame metadata,
then publishes one fixed-size completion descriptor through SPA's
cache-line-isolated SPSC ring. The PipeWireAO RTC data loop is the sole consumer
and remains responsible for validating and publishing the frame.

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

## Timing and metadata

`SPA_META_Header.seq` uses the GenTL frame ID and marks a discontinuity when the
sequence is not consecutive. `SPA_META_Header.pts` is the local
`CLOCK_MONOTONIC` time immediately before publication. Acquisition metadata is
valid and initialized, but this version does not claim an exposure-start time,
hardware acquisition identity, clock mapping, or uncertainty.

The source is complete-only. It does not offer progressive publication or
DMA-BUF. Progressive acquisition requires a producer-specific contract such as
eGrabber's StartOfCameraReadout and filled-size queries; BGAPI2's complete-buffer
callback does not provide that contract.

## Qualification

The factory test verifies load and parameter validation without opening a
camera. Camera and source tests use four or eight external host buffers,
respectively. The source test captures ten frames, returns every subscriber
lease, pauses and restarts halfway through the run, and performs ordered
teardown.

On 2026-08-23 all five tests passed against the connected 640x480 Mono8 GE34GM
camera through both:

```text
/opt/euresys/egrabber/lib/x86_64/gigelink.cti
/opt/baumer-gapi-sdk-c/lib/libbgapi2_gige.cti
```

The second result is significant: the Baumer producer does operate this camera.
An earlier failure was caused by an uncaught GenApi exception from the Euresys
producer, which terminated the shared process before the Baumer case ran.

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

## Remaining work

- Add manager and device factories for live camera discovery and reconciliation.
- Expose typed GenICam controls through `SPA_PARAM_PropInfo` and
  `SPA_PARAM_Props` for GUI discovery and paused control writes.
- Map a hardware or producer timestamp into the PipeWireAO acquisition clock
  contract before claiming exposure timing.
- Extend pixel-format coverage where a deterministic direct SPA mapping exists.
- Run open-loop latency and tail-jitter qualification at the intended camera
  rates and scheduling profiles.
- Decide whether a dedicated SDK-owning requeue agent is justified for strict
  zero-allocation operation. It should only be added with evidence that its
  extra handoff and scheduling cost improves the end-to-end deadline result.
