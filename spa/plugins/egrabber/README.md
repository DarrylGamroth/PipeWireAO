# eGrabber SPA plugin

This optional plugin is the native PipeWireAO integration for Euresys
eGrabber. Enable it with `-Degrabber=enabled`; use `-Degrabber-prefix=PATH`
when the SDK is not installed under `/opt/euresys/egrabber`. A disabled build
does not inspect or include the proprietary SDK.

## Implemented slice

The `api.egrabber.source` factory currently provides complete-frame capture and
mapped-host progressive publication:

- `api.egrabber.enum.manager` snapshot discovery and one
  `api.egrabber.device` object per discovered camera;
- standard device-to-source object creation with stable selector, vendor,
  model, serial, user-ID, and transport properties;
- standard SPA node, port, format, buffer, metadata, I/O, and command methods;
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

The factory test loads and enumerates the plugin without opening hardware. The
device test discovers the connected producer and verifies the standard
manager-to-device-to-node property chain; it skips when no camera is present.
Discovery is a construction-time snapshot, not a live-hotplug claim. The
capture test skips when no selected camera is available. With the connected
Gigelink camera it negotiates the live format, announces eight aligned
PipeWireAO-owned buffers, captures ten frames, validates Acquisition metadata,
returns every subscriber lease, and performs ordered teardown.

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
DMA-BUF source.

This is functional complete-mode evidence, not strict real-time admission.
The imported camera facade still takes uncontended mutexes around SDK queries,
and earlier hardware profiling found repeated allocations inside `gigelink.cti`
and `libegrabber` buffer-information calls. Those vendor and facade costs must
be removed or bounded before this source is admitted to a strict BusySpin
profile.

## Remaining migration

- Add serialized live discovery and removal events to the SPA manager.
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
- Integrate SDK readiness with EventFd and Hybrid RTC wait policies without
  adding a readiness syscall to BusySpin.
- Run the source through the PipeWireAO host-owned RTC loop and multiprocess
  fan-out qualification, then retire the standalone application.
