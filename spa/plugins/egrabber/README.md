# eGrabber SPA plugin

This optional plugin is the native PipeWireAO integration for Euresys
eGrabber. Enable it with `-Degrabber=enabled`; use `-Degrabber-prefix=PATH`
when the SDK is not installed under `/opt/euresys/egrabber`. A disabled build
does not inspect or include the proprietary SDK.

## Implemented slice

The `api.egrabber.source` factory currently provides complete-frame capture and
mapped-host progressive publication:

- `api.egrabber.enum.manager` off-loop serialized discovery with a one-snapshot
  handoff for reconciliation, and one
  `api.egrabber.device` object per discovered camera, with standard add,
  property-update, and removal events from a non-RTC loop timer;
- standard device-to-source object creation with stable selector, vendor,
  model, serial, user-ID, and transport properties;
- standard SPA node, port, format, buffer, metadata, I/O, and command methods;
- explicit `Mono8` and little-endian `Mono10`, `Mono12`, `Mono14`, and
  `Mono16` format support without starting acquisition during source
  construction;
- typed scalar GenICam discovery and readback through `SPA_PARAM_PropInfo` and
  `SPA_PARAM_Props`;
- immutable `SPA_NODE_FLAG_RTC_PROCESS` ownership;
- `SPA_IO_BuffersLatestLink` fan-out without graph-ready callbacks;
- mapped `MemPtr` or `MemFd` buffers announced directly to eGrabber;
- optional StartOfCameraReadout progressive publication on Grablink and
  Coaxlink, with whole-row release publication from `BUFFER_INFO_SIZE_FILLED`,
  immutable active metadata, and explicit complete or aborted terminal state;
- optional complete-frame DMA-BUF announcement when both eGrabber and a local
  DRM render node support it, using negotiated `SPA_META_SyncTimeline` acquire
  and release points rather than implicit synchronization;
- fixed `SPA_META_Header` and Version 1 `SPA_META_Acquisition` publication;
- monotonic `SPA_META_Header.pts` mapping from the vendor camera timestamp,
  reset on every Start and marked discontinuous when the camera clock resets or
  departs from the local monotonic anchor;
- configurable acquisition domain, initial generation, and
  StartOfCameraReadout context selection for Grablink/Coaxlink sources; the
  source advances generation on restart or a non-increasing trigger sequence
  and mirrors a valid acquisition sequence into `SPA_META_Header.seq`;
- bounded pool scans and explicit reclaim of only an unclaimed visible
  submission when the camera has no queued buffer; and
- synchronous camera stop and buffer release before pool teardown.

The plugin uses `spa_image_source`, `spa_image_source_latest`, and
`spa_buffer_latest` directly. It has no `pw_stream`, libpipewire client,
private mailbox, payload copy, worker thread, or graph scheduling path.

The camera, control-backend, frame-layout, frame-sequence, and pixel-format
code was migrated from sibling `egrabber-pipewire` revision `a3089cb`. The
standalone application remains the behavior oracle until the plugin reaches
parity. The migrated event bridge no longer copies `std::function` callbacks
per event, and the node tracks queued buffers locally instead of querying the
SDK on every empty RTC poll.

Control writes are serialized on the SPA control path and are never performed
by `process()`. Version 1 accepts one scalar write per `SPA_PARAM_Props` object.
The node must be paused for every write. A layout-changing write also requires
the output pool to be released; after it succeeds, the old Format is
invalidated and EnumFormat, Format, and Buffers are marked serial so the host
must renegotiate before the next Start. Writes while running or while a layout
is still bound return `-EBUSY` without changing the camera.

## Current qualification

The factory test loads and enumerates the plugin without opening hardware. A
source retains `EGrabberDiscovery` only until its selected `EGrabber` has been
constructed; it does not retain exclusive discovery-list authority or capture
a probe frame during initialization. The device test discovers the connected
producer, verifies an unchanged rescan
emits no duplicate object, and checks the standard manager-to-device-to-node
property chain; it skips when no camera is present. A normal PipeWireAO host
supplies loop utilities and receives discovery snapshots from a control-plane
thread. The thread waits until the SPA loop consumes its single completed
snapshot and then waits one second before the next scan, so slow vendor
discovery cannot occupy the SPA loop or accumulate work. A minimal host without
loop utilities retains synchronous discovery and `sync()` behavior. The
capture test skips when no selected camera is available. With the connected
Gigelink camera it
negotiates the live format, announces eight aligned
PipeWireAO-owned buffers, captures ten frames, validates Acquisition metadata,
returns every subscriber lease, and performs ordered teardown.

The opt-in host qualification runs the standard factory through an isolated
PipeWireAO daemon:

```console
./spa/plugins/egrabber/qualify-host.py build
```

It starts two remote `pw_stream` input processes against one mapped pool. The
first process retains its initial lease while the second joins, captures, and
leaves; the first then continues. A later subscriber replaces the inactive
pool through normal SPA renegotiation. Finally, the harness destroys the source
while that subscriber retains a lease and verifies that its metadata and
payload remain unchanged, the source disappears, and the daemon remains
healthy. The harness removes its isolated runtime directory on success or
failure.

Gigelink is complete-only: `progressive=offer` falls back to complete frames and
`progressive=require` is rejected. Grablink/Coaxlink progressive behavior is
implemented against the vendor StartOfCameraReadout, acquiring-buffer, and
filled-size contract, but remains a hardware qualification item. Progressive
publication rejects DMA-BUF by design.

Explicit-sync DMA-BUF is currently restricted to one active subscriber. The
standard SyncTimeline allocation has one release timeline, so it cannot safely
represent several independent asynchronous consumers of a shared fan-out
buffer. Mapped host buffers retain normal PipeWireAO fan-out. A second live
subscriber is rejected before capture starts and cannot join a running
DMA-BUF source. Release readiness is queried without waiting on the RTC path.
A slot whose release point has not been signalled remains locally held while
the bounded scan examines the rest of the pool; a late or failed subscriber
therefore produces pool starvation rather than blocking acquisition.

This is functional complete-mode evidence, not strict real-time admission.
The imported camera facade still takes uncontended mutexes around SDK queries,
and earlier hardware profiling found repeated allocations inside `gigelink.cti`
and `libegrabber` buffer-information calls. Those vendor and facade costs must
be removed or bounded before this source is admitted to a strict BusySpin
profile.

The eGrabber CallbackOnDemand API exposes no readiness file descriptor. The
plugin therefore has no honest EventFd or Hybrid readiness source and does not
add a helper thread or private handoff merely to synthesize one. PipeWireAO's
RTC data loop implements EventFd and Hybrid for SDKs that provide pollable
readiness; this plugin currently uses its functional BusySpin profile only.

## Remaining migration

- Qualify physical camera add, property-change, removal, and reappearance with
  each supported producer. Automated tests currently cover initial discovery
  and unchanged reconciliation on the connected Gigelink producer; the normal
  daemon qualification exercises the asynchronous snapshot handoff.
- Qualify acquisition-domain identity on Grablink/Coaxlink hardware and
  physical exposure-start mapping and uncertainty. The current completion-time
  anchor restores generic Header PTS behavior but does not claim an
  `SPA_META_Acquisition.exposure_start` value.
- Qualify StartOfCameraReadout progressive publication on Grablink/Coaxlink
  hardware, including partial-row, completion, incomplete-frame, restart, and
  cancellation behavior.
- Qualify complete-frame DMA-BUF and SyncObj timeline behavior on supported
  Grablink/Coaxlink hardware. The connected Gigelink device cannot exercise
  this path.
- Remove or bound the known vendor/facade allocations and locks before
  admitting the eGrabber process function to a strict BusySpin deployment.
- Keep the standalone application only as a physical Grablink/Coaxlink
  progressive and DMA-BUF behavior oracle until those paths are qualified in
  the SPA plugin.
