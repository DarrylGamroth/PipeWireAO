# Bounded queue module

Status: implemented and live-graph tested; deployment latency qualification
remains open

Decision: PWAO-PLUGIN-003

Baseline: `pipewireao-spa-plugins` `main` at `c6b7633`

Applicable profiles: complete-buffer `application/ndarray` and `video/raw`
streams on Linux

## Purpose and authority

This document is the normative contract for the native
`libpipewire-module-queue` module. It owns the queue configuration vocabulary,
capacity and overload behavior, payload-storage modes, lease lifetime,
execution boundary, lifecycle, and counters.

The negotiated ndarray, raw-video, and metadata ABIs remain owned by PipeWireAO.
Device and algorithm plugins remain responsible for the semantic validity of
the products they publish. Deployment configuration owns graph-driver and data
loop placement. This module does not define a scientific product, clock, or
scheduling policy.

Uppercase requirement terms use the meanings defined by BCP 14 (RFC 2119 and
RFC 8174). Lowercase forms are ordinary prose.

## Terminology

- A **queued buffer** is a complete input buffer accepted by the module but not
  yet claimed by its output side.
- An **in-flight buffer** has been published by the output side and has not yet
  been returned by its downstream consumer.
- **Backpressure** means that the module retains capacity and stops completing
  input work. It does not mean that a thread waits in a blocking system call.
- **Copy storage** copies the selected complete payload once, on the output
  side, into the output link's buffer pool.
- **Lease storage** makes the output pool refer to the input payload storage
  and retains the input buffer until the corresponding output buffer returns.
- A **replacement** occurs when `drop-oldest` removes the oldest queued buffer
  to admit a new buffer.

The public term is `overflow`, not `leaky`. GStreamer's `leaky` property is an
informative precedent, but it is not a compatibility boundary and its
directional values are not used here.

## Configuration

| Property | Required value |
| --- | --- |
| `queue.max-buffers` | Decimal integer from 1 through 62. |
| `queue.overflow` | `backpressure`, `drop-oldest`, or `drop-newest`. |
| `queue.storage` | `copy` or `lease`. |
| `queue.media` | Optional `application/ndarray` (default) or `video/raw`. |
| `metadata` | Optional cold array of exact `{type,size,required}` fact layouts. Header is implicit and required. |
| `node.name` | Optional base name for generated endpoint names and group-title fallback. |
| `node.description` | Optional exact presentation title for the queue group. |
| `capture.props` | Optional PipeWire properties for the input stream. |
| `playback.props` | Optional PipeWire properties for the output stream. |

There are no implicit aliases or silent mode fallbacks. In particular,
`latest`, `leaky`, `zero-copy`, and GStreamer-style `upstream` or `downstream`
values are not accepted configuration vocabulary.

The recommended observer-isolation profile is:

```ini
queue.max-buffers = 1
queue.overflow = drop-oldest
queue.storage = copy
queue.media = application/ndarray
```

`queue.storage = lease` selects the bounded zero-payload-copy alternative.

For row-block camera processing, place the queue after complete-frame
assembly, not between row-block producers and the assembler:

```text
eGrabber -> pixel calibration -> frame assembly -> capacity-one queue -> GUI
```

This makes the queue's input a complete immutable frame ndarray, as required
by QUEUE-001, and leaves row-block sequencing inside the real-time graph.

A PipeWire configuration fragment can load the module as follows:

```ini
context.modules = [
    {
        name = libpipewire-module-queue
        args = {
            node.name = telemetry-queue
            node.description = "Telemetry Queue"
            queue.max-buffers = 1
            queue.overflow = drop-oldest
            queue.storage = copy
            queue.media = application/ndarray
            capture.props = {
                node.name = telemetry-queue-input
            }
            playback.props = {
                node.name = telemetry-queue-output
            }
        }
    }
]
```

The input and output are ordinary PipeWire nodes and must be linked by the
deployment or session manager. Both endpoint nodes exist for the lifetime of
the module. Before the first input negotiation, the output advertises no
usable complete-buffer format. During a later input-pool transition it retains
the last exact contract but publishes no frame. Once the next input pool is
prepared, PipeWire replaces the output buffer pool in place without replacing
the output endpoint or its compatible link. An installed output buffer's data
descriptors remain immutable for that buffer-pool generation.

The module publishes the same opaque `pipewireao.queue.id` property on both
endpoint nodes. Graph clients MUST use that identity when they need to relate
the output endpoint to its input endpoint; client identity, media name, node
group, and link group are not unique queue identities. The value is owned by
the module instance and is not a configuration property.

## Graph presentation metadata

Both endpoint nodes MUST publish the following module-owned presentation fields:

| Property | Published value |
| --- | --- |
| `pipewireao.graph.group.id` | An opaque randomly generated 128-bit instance identity, shared by both endpoints. |
| `pipewireao.graph.group.name` | The first nonblank top-level `node.description`, then `node.name`, otherwise `Queue`. |
| `pipewireao.graph.group.class` | `Queue`. |

An absent, empty, or whitespace-only top-level title is blank. The group title
MUST preserve the selected nonblank string exactly, including leading and
trailing spaces. In particular, the module MUST NOT derive the title from an endpoint name or remove numeric
suffixes. Each endpoint's `node.description` MUST preserve an explicitly
configured endpoint value; otherwise it MUST equal that endpoint's exact
`node.name`. Thus the shared presentation title and individual endpoint labels
have distinct owners.

The module MUST overwrite all three group fields in `capture.props` and
`playback.props`, even when those values conflict. Callers choose the shared
title through top-level `node.description` or `node.name`. Both endpoints of a
queue MUST share one group identity, and simultaneously existing queue instances
MUST have different group identities even when their configured titles match.
Clients MUST treat the identity as opaque and connection-lifetime scoped; it is
not a persistent configured name. `pipewireao.queue.id` remains the queue's
protocol identity for endpoint association. The generic graph group fields let
presentation clients group nodes without knowing the queue module or a device's
vocabulary.

For `queue.storage=copy` with `queue.overflow=drop-oldest`, at any valid capacity,
the module MUST default both endpoint `media.role` properties to `Analysis`.
An explicitly configured endpoint `media.role` MUST be preserved. Other storage
or overflow profiles MUST NOT acquire this role by default. This classification
identifies the usual observer-isolation profile; it does not establish that all
downstream consumers are passive observers. Graph clients can combine the role
with actual consumer topology, keeping processing consumers visible.

Presentation groups do not set execution ownership, graph scheduling, or link
grouping. The separate `node.group` and `node.link-group` properties retain their
execution semantics. Native-protocol clients can obtain these presentation
fields from bound Node info properties, including on servers whose registry
global properties do not forward custom fields.

## Boundary and execution model

```mermaid
flowchart LR
    Producer["Critical producer graph"]
    Input["Queue input stream<br/>producer data loop"]
    Ring["Bounded queue<br/>preallocated leases"]
    Output["Queue output stream<br/>observer data loop"]
    Observer["Telemetry, GUI, or recorder"]
    Pool["Observer output pool"]

    Producer -->|complete buffer| Input
    Input -->|publish lease| Ring
    Ring -->|claim| Output
    Output -->|lease storage| Observer
    Output -->|copy storage| Pool
    Pool --> Observer
```

The input and output streams are separate PipeWire nodes. The input process
callback executes with the producer graph. The output process callback executes
with the observer graph. Publication does not wake the output graph; the
observer graph provides its own pacing. This deliberate polling boundary avoids
an eventfd write or another notification system call in the producer callback.

`backpressure` uses an asynchronous reverse-loop invocation when output
progress makes room for a retained arrival. That recovery can wake the producer
data loop. It does not run on the producer publication path, but it is another
reason that `backpressure` is not the observer-isolation profile.

For a complete-frame camera observer, the queue forwards the source's exact
raw-video format without converting it:

```text
eGrabber Video/Source -> capacity-one queue -> GUI video observer
```

That deployment MUST set `queue.media=video/raw`. Selecting one media family
at module creation keeps input format negotiation deterministic; the exact
format details are still copied from the source to the output.

### QUEUE-001 — Ordinary complete-buffer boundary

The module MUST expose one input stream and one output stream using ordinary
PipeWire complete-buffer transport. It MUST accept the media family selected
by `queue.media`, negotiate one exact `application/ndarray` or `video/raw`
format on the input, and advertise that exact format on the output. It MUST
NOT require a private buffer-ownership protocol on either side.

Verification intent (informative): inspect stream flags and negotiated PODs;
exercise exact ndarray and raw-video formats through ordinary stream I/O.

### QUEUE-002 — Finite capacity and admission

The module MUST reject `queue.max-buffers` outside 1 through 62. For capacity
`N`, it MUST request and validate at least `N + 2` input buffers before
streaming. It MUST retain no more input buffers than the negotiated finite
pool, including queued, in-flight, completing, and transport-held buffers.

Verification intent (informative): boundary tests for 0, 1, 62, and 63;
saturation tests that account for every negotiated input buffer.

### QUEUE-003 — Overflow policy

When the queue is full, the module MUST apply exactly the selected
`queue.overflow` policy:

- `backpressure` MUST preserve queued order and MUST NOT discard a queued or
  arriving buffer as an overflow action;
- `drop-oldest` MUST release the oldest queued input buffer and admit the
  arriving buffer; and
- `drop-newest` MUST release the arriving input buffer without changing the
  queued buffers.

The module MUST preserve FIFO order among buffers that are neither replaced nor
dropped. A drop policy MUST NOT wait for output capacity.

Verification intent (informative): deterministic capacity-one and multi-slot
tests for all policies, including recovery after pressure subsides.

### QUEUE-004 — Storage modes

With `queue.storage=copy`, the input process callback MUST NOT copy payload
bytes. The output process callback MUST copy each delivered payload at most once
into an output-link buffer and MUST release the corresponding input lease after
the copy completes.

With `queue.storage=lease`, the module MUST NOT copy payload bytes. It MUST
admit only stable shareable input storage that can back the corresponding
output buffers, and it MUST retain each input lease until its output buffer is
returned. Unsupported storage MUST fail configuration; the module MUST NOT
fall back to copy storage.

Verification intent (informative): pointer or file-descriptor identity checks
for lease storage, mutation isolation for copy storage, and negative admission
tests for unshareable storage.

The initial implementation accepts at most eight data blocks and 64 metadata
records per buffer. Copy storage requires CPU-mapped input data. Lease storage
requires every data block to be a shareable `MemFd` or `DmaBuf`. Explicit
`SyncTimeline`/`SyncObj` DMA synchronization is rejected in this version;
supporting it requires a separate synchronization contract, not a silent
fallback.

### QUEUE-005 — Producer repeated path

After preparation, the module's input process callback MUST use bounded work
and preallocated state. Its ordinary publication and drop paths MUST NOT
allocate memory, copy payload bytes, acquire a blocking lock, read a clock,
format or emit a log message, or directly invoke a file-descriptor system call.
One input callback MUST inspect no more buffers than the negotiated input-pool
cardinality.

Verification intent (informative): source inspection, allocator interposition,
bounded-work tests, and syscall tracing around a warmed producer path.

### QUEUE-006 — Format, metadata, and gaps

The output MUST preserve the exact negotiated format, each negotiated
application metadata record, every data-block chunk description, and the
complete payload selected for delivery. Sequence and acquisition metadata MUST
remain unchanged. PipeWire's transport-owned `SPA_META_Busy` record MUST remain
link-local and MUST NOT be copied between the input and output links.
Replacement and drop counters MUST make lost sequence positions observable;
the module MUST NOT renumber products to hide a gap.

Verification intent (informative): multi-block payload, Header, independent records,
chunk, and sequence-gap tests in both storage modes.

Capture and playback default to required standard Header only. An explicit cold
scientific profile declares each independent PipeWireAO record as a separate
exact-size ParamMeta capability. Both endpoints share that bounded capability set. Its cold replacement pool preserves every
accepted source record with its exact type and size; missing output storage
rejects preparation or publication. Custom types use `SPA_PARAM_META_required`
allocation negotiation rather than the standard 32-bit metadata mask.

An explicit `metadata` array replaces the default additional records and supplies
one shared capture/playback capability set. For example,
`metadata = [ { type = 1022 size = 8 required = true } ]` negotiates an opaque
extension without changing a central type switch. Type, size and required Bool
are all explicit; unknown fields and duplicate keys or types fail configuration.
A consistent Header declaration is allowed once. Retired IDs, mutable transport
state, incompatible known sizes, more than 64 total entries or more than 4096
aligned bytes fail cold validation. Both peers must declare custom layouts;
required missing layouts fail before publication. Queue endpoints default Header
PTS clock and absolute conversion to `unavailable`; explicit endpoint cold
properties can describe a known presentation timeline without inventing event time.

COPY and LEASE preserve accepted opaque optional payloads when negotiated output
storage exists. Transfer validates count, aligned aggregate bytes, singleton
duplicates and understood record semantics before changing metadata or chunks.
Busy, SyncTimeline and zero-size feature entries remain transport-owned and link-local. Reuse invalidates stale
output records. Optical chopping and electrical trigger classification remain
independent; neither classification is derived from the other. Consumers validate
their required facts on each input, independently of allocation negotiation.
The native 4.x profile uses 24-byte ExposureTiming and ReferenceTimestamp
(type 0x305) records. Reference timestamps retain unsigned integer nanoseconds;
UINT64_MAX is the unspecified-duration sentinel. Retired metadata IDs 11, 12,
13 and 0x302, and the former 64-byte ExposureTiming layout, are unsupported.

### QUEUE-007 — Lifecycle and lease recovery

Pause, format removal, stream removal, module destruction, and configuration
failure MUST prevent new publication. The module MUST empty its pending and
completion queues and MUST account for every retained input and output lease
before the corresponding pool storage can be reused or released. A clean pause
MAY retain the current negotiated pool, but restart MUST begin with an empty
pending queue and MUST preserve cumulative counters. Format or pool withdrawal
starts a new input-pool generation.

A copy queue with no playback buffers returns its outstanding capture buffers
in the capture pool's removal callback. This includes capture withdrawal after
the playback pool has been removed. The capture data loop serializes these
returns with capture processing and pause handling. On PipeWireAO 4.0.8,
`pw_stream` emits the pool's buffer removal callbacks before announcing input
format removal.

In lease storage, the output buffer MUST own an independent reference to the
shared MemFd or DmaBuf and MUST NOT expose a pointer whose mapping lifetime is
owned by the input stream. The module MUST NOT return an input buffer for reuse
while a live output buffer still leases that input storage. Input-pool
withdrawal MAY revoke the input buffer before the queue's output buffer is
returned; in that case the independent output storage reference MUST keep the
published payload valid until that output buffer is returned or its output pool
is withdrawn. A downstream client MUST honor its own `remove_buffer` callback;
this requirement does not extend the lifetime of an application-held
`pw_buffer` after the downstream pool has revoked it.

Verification intent (informative): stop and destroy at empty, queued,
in-flight, completing, and backpressured states; retain a lease across input
pool withdrawal while the output pool remains current; verify the retained
bytes remain valid; cover both returning it before downstream pool
renegotiation and reconnecting the input while it remains held; in the latter
case verify that downstream observes output-pool revocation before the new
generation is delivered, then restart without stale ownership or descriptors.

### QUEUE-008 — Observability

The module MUST maintain monotonic counters for input publications,
deliveries, replacements, dropped arrivals, backpressure observations,
completions, output-pool exhaustion, and protocol errors. Counter updates on a
process path MUST be bounded and MUST NOT log per event.

Verification intent (informative): compare counters with deterministic event
histories and saturation scenarios.

### QUEUE-009 — Observer isolation claim

The `drop-oldest` and `drop-newest` policies MUST bound the input leases
retained by a stalled observer according to QUEUE-002. Copy storage MUST release
an input lease after the output-side copy, so subsequent downstream retention
uses only the observer output pool. Lease storage MAY retain one input lease
for the in-flight output in addition to queued leases.

Backpressure is not an observer-isolation policy: a stalled observer is allowed
to exhaust the finite input pool and stall its producer graph.

Verification intent (informative): deliberately stall the output for each
policy and storage mode and account for retained leases and producer progress.

### QUEUE-010 — Placement and claim limits

The module MUST use distinct default node groups for its input and output
streams. A deployment claiming control-path isolation MUST place the output
stream and observer outside the critical producer's driver group and data loop.
Functional or unit evidence MUST NOT be presented as proof of a latency bound,
system-call absence inside PipeWire itself, scheduler isolation, or hardware
performance.

Verification intent (informative): inspect the live graph and thread placement;
use a fixed-arrival latency qualification with one deliberately stalled
observer before promoting an operational isolation claim.

### QUEUE-011 — Stable endpoints and transparent restart

The module MUST create one input endpoint and one output endpoint during module
initialization. Their PipeWire object identities, configured node names, and
`pipewireao.queue.id` value MUST remain unchanged across an absent input
format, input pause, buffer-pool withdrawal, format renegotiation, and clean
restart. A downstream link compatible with the newly negotiated exact format
MUST resume without the client resolving a replacement queue node or creating
a replacement link.

Verification intent (informative): record both endpoint object identities,
attach a downstream observer, remove and recreate the producer link and buffer
pool, then verify the same endpoint and link objects deliver the new generation.
Repeat through a remote daemon and registry client.

### QUEUE-012 — Failure containment

An input or output link negotiation failure, incompatible downstream format,
or downstream removal MUST NOT destroy the module or the unaffected endpoint.
An internal ownership invariant failure MUST stop publication, retain the first
failure diagnostics, and put both endpoints into an error state. Loss of the
module's PipeWire core connection remains terminal for the module instance.

Verification intent (informative): attach an incompatible observer and remove
it; verify the queue endpoints remain registered and a later compatible
observer receives data. Inject an ownership failure separately and verify the
fail-fast diagnostic path.

## Ownership and publication

The input callback is the single producer for the pending ring. The output
callback is its single consumer. Both may advance the pending read position:
the output advances it when claiming the oldest buffer, and the producer may
advance it only to implement `drop-oldest`. The implementation therefore uses
an atomic compare-and-exchange to assign each oldest slot to exactly one side.

The input callback is the single consumer of the completion ring. The output
callback is its single producer. A release publication of the pending write
position makes the initialized slot visible to an acquire-reading output. A
successful acquire-release claim of the pending read position transfers the
slot lease. The completion ring uses the same release/acquire publication
direction in reverse.

Payload mutation completes before the upstream producer publishes the input
buffer to PipeWire. This module treats the input lease as immutable. Copy
storage creates an observer-owned mutable copy before output publication;
lease storage preserves immutability until downstream release.

The control path assigns lease descriptors only from the output stream's
`add_buffer` callback. It publishes a generation as ready only after every
output buffer has been installed and validated against the current input pool.
An input-generation change clears readiness before the data loops are
quiesced. Returned old-generation buffers remain unavailable until PipeWire
removes that pool; neither process callback closes, duplicates, or changes an
installed data descriptor.

A copy queue with no playback buffers has one additional retirement step.
PipeWire 4.0.8 detaches the capture port from its data loop and clears its
I/O before issuing the first stream `remove_buffer` callback. During this
callback the control thread can wait for the capture-loop mutex.
`pw_loop_locked` runs the returns inline under that mutex while the native
buffers can still be queued. Completion handling is serialized with the
returns, and an absent or already retired playback pool has no copy in flight.
Deferring the returns past the callback would lose the native loan when
PipeWire clears the pool.

Pause and pool-withdrawal transitions use an atomic publication gate and
enqueue non-blocking stages in playback-loop, capture-loop, and main-loop
order. Each stage therefore follows any callback already executing on its own
loop without acquiring or waiting for another loop's lock. Process callbacks
return while the gate or a requested transition is present. The final main-loop
stage either starts a stronger pending transition or reopens publication and
prepares a complete replacement pool.

The pending read index, pending write index, queue storage, input counters,
output counters, and fatal-error flag occupy separate cache-line-aligned
regions. Module state is allocated at the same alignment. This layout avoids
unrelated producer/observer counter traffic sharing the publication cache
lines; it does not eliminate the cache transfer required to publish or claim a
slot.

The local ring deliberately preserves this cache-line isolation without using
`spa_ringbuffer_shared`, which is a PipeWireAO extension rather than an
upstream SPA ABI. Upstream `spa_ringbuffer` keeps its two indices adjacent.
Its API also assumes that only the consumer writes the read index, whereas
`drop-oldest` requires the queue producer and consumer to claim the oldest
position with compare-and-exchange. The ordinary completion ring could use an
upstream SPA ringbuffer, but doing so would retain the custom ring for pending
items, introduce two queue mechanisms, and lose index isolation on that path.

## Counters

The module publishes cumulative decimal counters as module properties and on
both endpoint nodes once at initialization and then once per second:

| Property | Meaning |
| --- | --- |
| `queue.stats.publications` | Complete input buffers observed. |
| `queue.stats.replacements` | Queued inputs displaced by `drop-oldest`. |
| `queue.stats.dropped-arrivals` | Arriving inputs discarded by `drop-newest`. |
| `queue.stats.backpressure` | Arrivals retained because the queue was full. |
| `queue.stats.withdrawal-returns` | Capture buffers returned during input-pool withdrawal when no copy playback buffers remain. |
| `queue.stats.deliveries` | Buffers published on the output stream. |
| `queue.stats.completions` | Input leases completed by the output side. |
| `queue.stats.pool-exhaustions` | Output cycles without the required reusable copy buffer or current-generation lease buffer. |
| `queue.stats.protocol-errors` | Ownership or layout invariant failures. |

Current bounded state is also published once per second. These values are
diagnostic gauges, not cumulative counters:

| Property | Meaning |
| --- | --- |
| `queue.state.pending-depth` | Current pending-ring depth. |
| `queue.state.completion-depth` | Current completion-ring depth. |
| `queue.state.active-outputs` | Output buffers awaiting return. |
| `queue.state.capture-buffers` | Current input-pool buffer count. |
| `queue.state.playback-buffers` | Current output-pool buffer count. |
| `queue.state.configured` | Whether the current input pool has prepared the output contract. |
| `queue.state.input-format` | `negotiated` or `none`. |
| `queue.state.ownership-transition` | Ownership lifecycle phase: `idle`, `pause`, `withdraw`, or `finishing`. |
| `queue.state.output-generation` | Output-pool phase: `none`, `waiting`, or `ready`. |
| `queue.state.capture-generation` | Current input-pool generation, or zero before the first pool. |
| `queue.state.requested-generation` | Input generation requested from output negotiation, or zero when none is pending. |
| `queue.state.installed-generation` | Generation of the complete usable output pool, or zero while it is unavailable. |

The module preserves the first ownership failure in diagnostic properties
before putting its streams into the error state. The daemon also logs the same
record so the failure remains available even if an operator later unloads the
failed module.

| Property | Meaning |
| --- | --- |
| `queue.error.operation` | Stable name of the ownership operation that failed, or `none`. |
| `queue.error.source-line` | Source line at which that operation reported the failure. |
| `queue.error.slot` | Affected queue slot, or `n/a` when no slot was involved. |
| `queue.error.expected-state` | Required slot ownership state. |
| `queue.error.observed-state` | Slot ownership state actually observed. |
| `queue.error.pending-depth` | Pending-ring depth captured at the first failure. |
| `queue.error.completion-depth` | Completion-ring depth captured at the first failure. |
| `queue.error.blocked-slot` | Backpressured input slot at the first failure, or `n/a`. |
| `queue.error.slot-acquisitions` | Successful acquisitions of the affected slot in its current pool. |
| `queue.error.slot-returns` | Successful upstream returns of the affected slot in its current pool. |
| `queue.error.expected-token` | Required generation-tagged slot token, or `n/a`. |
| `queue.error.observed-token` | Generation-tagged slot token actually observed, or `n/a`. |
| `queue.error.result` | Negative errno-style result associated with the failure. |

Counters are monotonic for the module lifetime. Pause and restart clear queued
ownership but do not reset them.

## Non-goals

- An unbounded queue or dynamically growing repeated-path storage.
- Byte, duration, or watermark capacity in the first profile.
- A producer-side notification, eventfd write, timer, or busy-wait loop.
- Partially committed payloads.
- Semantic joins, timestamp rendezvous, zero-order hold, or missing-input
  policy.
- A claim that zero-copy is faster than copy storage without measurement.
- Compatibility with GStreamer queue property names.

The [original plugin verification record](https://github.com/DarrylGamroth/pipewireao-spa-plugins-core/blob/01df204/docs/queue.md#current-verification-boundary) covers the out-of-tree module before its move into PipeWireAO.
