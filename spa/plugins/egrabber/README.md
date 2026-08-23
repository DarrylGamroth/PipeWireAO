# eGrabber SPA plugin

This optional plugin is the native PipeWireAO integration for Euresys
eGrabber. Enable it with `-Degrabber=enabled`; use `-Degrabber-prefix=PATH`
when the SDK is not installed under `/opt/euresys/egrabber`. A disabled build
does not inspect or include the proprietary SDK.

## Implemented slice

The `api.egrabber.source` factory currently provides complete-frame capture:

- `api.egrabber.enum.manager` snapshot discovery and one
  `api.egrabber.device` object per discovered camera;
- standard device-to-source object creation with stable selector, vendor,
  model, serial, user-ID, and transport properties;
- standard SPA node, port, format, buffer, metadata, I/O, and command methods;
- immutable `SPA_NODE_FLAG_RTC_PROCESS` ownership;
- `SPA_IO_BuffersLatestLink` fan-out without graph-ready callbacks;
- mapped `MemPtr` or `MemFd` buffers announced directly to eGrabber;
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

## Current qualification

The factory test loads and enumerates the plugin without opening hardware. The
device test discovers the connected producer and verifies the standard
manager-to-device-to-node property chain; it skips when no camera is present.
Discovery is a construction-time snapshot, not a live-hotplug claim. The
capture test skips when no selected camera is available. With the connected
Gigelink camera it negotiates the live format, announces eight aligned
PipeWireAO-owned buffers, captures ten frames, validates Acquisition metadata,
returns every subscriber lease, and performs ordered teardown.

This is functional complete-mode evidence, not strict real-time admission.
The imported camera facade still takes uncontended mutexes around SDK queries,
and earlier hardware profiling found repeated allocations inside `gigelink.cti`
and `libegrabber` buffer-information calls. Those vendor and facade costs must
be removed or bounded before this source is admitted to a strict BusySpin
profile.

## Remaining migration

- Add serialized live discovery and removal events to the SPA manager.
- Expose scalar GenICam controls through `SPA_PARAM_PropInfo` and
  `SPA_PARAM_Props`, including fenced layout-changing writes.
- Qualify acquisition-domain identity on Grablink/Coaxlink hardware and
  physical exposure-start mapping and uncertainty. The current completion-time
  anchor restores generic Header PTS behavior but does not claim an
  `SPA_META_Acquisition.exposure_start` value.
- Add StartOfCameraReadout progressive publication for Grablink and Coaxlink;
  Gigelink remains complete-only.
- Add optional DMA-BUF complete capture and explicit synchronization.
- Integrate SDK readiness with EventFd and Hybrid RTC wait policies without
  adding a readiness syscall to BusySpin.
- Run the source through the PipeWireAO host-owned RTC loop and multiprocess
  fan-out qualification, then retire the standalone application.
