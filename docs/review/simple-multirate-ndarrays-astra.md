# Independent review: simple multirate ndarrays

Review date: 2026-09-12. Disposition: retain synchronous composite graphs; use
a separate simple latest/hold SPA rate-transition node as the baseline when
bounded copying per fast output is accepted. Defer exact acquisition joining
entirely from that baseline. The initial composite-private proposal does not
require a second scheduler, but its account of rates, arrivals, retention, and
backend parity omits several necessary choices.

## Scope and revision

| Item | Reviewed value |
| --- | --- |
| PipeWire revision | `ea9d90e27f1d4d6405f3466db57831512c7a0dc1`, branch `master` |
| Worktree | `/home/dgamroth/workspaces/codex/pipewire/pipewire` |
| Proposal | `doc/dox/internals/simple-multirate-ndarrays.md`, initial 340-line draft |
| Proposal SHA-256 | `db6e7f2f14ec13c76fe22f9e6d83f143cba8bd3e91ac1f10285f0e4c683725f4` |
| JuliaFilterGraph.jl revision | `54177fa887451ea9386f07bede18e9942a58fe76` |
| PipeWireAO.jl revision | `9c54326f78f5230d47ed33bbeb08479d322c79dc` |
| pipewireao-rtc revision | `17365a4250dd0a718dc45fbd7985d47afd1374f4` |
| pipewireao-spa-plugins-core revision | `ac82518cc876cb6fce8baf55fc6875f8fba0f71f`, clean when inspected |
| Pre-existing changes | Proposal untracked; `doc/dox/internals/index.dox` and `doc/meson.build` modified by the primary agent; sibling repositories clean when inspected |
| Review changes | This report only; existing test-log output was refreshed |

The review used the low-latency design skill and its architecture checklist.
No production changes, experimental patches, or new scheduling mechanisms were
introduced. The assigned shared worktree was used because the task was source
inspection plus one explicitly assigned report; design files were not edited.
Proposal line references below refer to the fingerprinted initial draft, even
if subsequent remediation moves them.

Evidence labels:

- **Observed:** established by inspected source or the named executed check.
- **Derived:** follows from that evidence under the stated conditions.
- **Hypothesis:** needs a live or hardware experiment; not a confirmed runtime defect.

This review does not independently verify CACAO or HEART internals. It evaluates
the requested pattern of one primary loop using current input and bounded held
state against the local PipeWire implementations.

## Assessment and minimum contract

One ordinary PipeWire composite containing a synchronous numerical graph is a
good boundary. Per-algorithm PipeWire nodes, a general scheduler, a trigger
service, and arbitrary timestamp windows are unnecessary for the stated first
example. A data-loop-owned latest value is ordinary application state and does
not become a scheduler merely because it gates numerical execution.

Following the additional requested comparison, the smallest useful shared
delivery is one selected 1000 Hz driver, one compatible 100 Hz source operating
as a follower, a single latest/hold rate-transition SPA node, an ordinary
synchronous composite, and one actuator sink. The hold node has one 100 Hz input
and one 1000 Hz output. In each fast graph cycle it ingests a new slow value if
available and copies the current valid value into an available output buffer.
The main source links directly to the composite; the hold node supplies its
other, now genuinely 1000 Hz, input. PipeWire dependency scheduling orders the
hold node before the composite. A slow arrival need not cause an extra cycle.

The node must never wait for the next slow sample and must not create a timer
or drive the graph. Describe the source's behavior when the selected driver
asks it to process without a new sample, and measure the up-to-next-cycle
admission delay. Do not assume all camera plugins have that behavior. Missing
or expired held state produces no usable output sample; a normal all-required
composite then publishes no command when that input is absent.

The stated 1000 Hz example gives a nominal 1 ms cycle interval. It does not
establish the permissible callback-to-command tail, exposure-to-application
latency, frame size, memory bandwidth, core count, or interference budget. Ten
primary uses of a held sample also do not imply a 10 ms wall-clock age bound if
primary acquisitions are skipped or stop. These are deployment inputs, not
evidence that the plan cannot work. No hard-real-time or hardware qualification
claim can follow from this design review.

| Critical-path stage | Owner and handoff | Required bound or decision |
| --- | --- | --- |
| Acquire/publish source sample | Source implementation and ordinary PipeWire pool | Nonblocking follower/no-sample behavior; number of leases; selected driver's cadence |
| Deliver current graph cycle | PipeWire dependency scheduler and port I/O | One selected driver; synchronous versus ASYNC links explicit |
| Admit and repeat slow input | Latest/hold node's data-loop thread | One persistent held value; ingest before publishing; no hidden wait |
| Process numerical graph | Prepared FGN or Julia owner | Synchronous completion; prepared runtime/FFI path; configured work and array sizes bounded |
| Publish command | Composite and output pool | Explicit missing-output behavior; acquisition metadata assigned from primary/join key |
| Consume command | Actuator sink | No implied atomic physical application; sink-specific delay and missing-command response |
| Observe counters | Data loop writes; control loop snapshots | Race-free snapshot; no per-frame registry traffic |

## Placement comparison and queue decision

| Choice | Additional behavior | Consequence | Recommendation |
| --- | --- | --- | --- |
| Composite-private ingress in C and Julia | Each host retains and admits its own held state; translates external/internal rates | Avoids a repeated PipeWire payload copy, but needs two policy integrations, parity work, and new native lease/lifecycle handling if leasing | Optimization only if the separate node's measured cost violates the selected budget |
| One latest/hold SPA node before either composite | One input cache; one copy per fast publication; explicit 100-to-1000 Hz transition | Both backends use their ordinary equal-rate synchronous graph interfaces; one semantic implementation is inspectable in PipeWire | Baseline with the accepted bounded-copy cost |
| Add latest/hold as a queue mode | Extend a two-endpoint transport, format contract, lifetime model, and empty-queue behavior | Mixes one-time queue delivery with repeated-value semantics and cadence policy | Do not use as the baseline |

The proposed hold node is comparable to the existing queue in the useful sense
that it is an explicit PipeWire boundary with fixed storage, normal buffers,
metadata, lifecycle, and counters. It is not a configuration of the current
queue. Queue contracts and source are authoritative for this distinction:

- `../pipewireao-spa-plugins-core/docs/queue.md:140-149` assigns capture and
  playback to separately paced PipeWire graphs; publication does not wake the
  output graph. `module-queue.c:2070-2080` gives the endpoints distinct default
  groups/link-groups. A single hold SPA node instead belongs to the fast
  component and has one process-state owner.
- QUEUE-001 (`docs/queue.md:162-168`) forwards the exact input format. QUEUE-006
  (`:237-245`) preserves sequence and acquisition metadata. A hold node
  deliberately changes nominal output sample rate while preserving the identity
  of the physical slow acquisition from which repeated values were made.
- `src/modules/queue/module-queue.c:801-920` claims one pending input, returns
  when none exists, and in copy mode completes the input lease after transfer.
  A capacity-one/drop-oldest queue therefore selects the newest *undelivered*
  sample; it has no persistent last-delivered sample to publish on the next nine
  fast cycles. A hold node must keep that value after delivery.
- QUEUE-004 (`docs/queue.md:201-212`) copies on delivery, with no producer-side
  payload copy. Copying a slow arrival into prepared hold storage and repeating
  it at the fast output uses a different ownership/copy contract. The queue's
  pending/completion rings and generation-tagged cross-loop lease states are
  unnecessary for a one-owner hold node.
- Zero-order hold and missing-input policy are expressly queue non-goals
  (`docs/queue.md:454-461`). Extending that contract is a deliberate new design,
  not an inexpensive reuse of a “latest” switch.

Recommendation: implement a separate simple SPA node in the existing external
plugin project. Reuse the queue's proven test patterns and, where their contracts
fit, its small buffer validation/copy mechanics. `buffer-transfer.c:14-93` and
`buffer-transfer.h:15-20` already validate transfer layout, bound blocks/metas,
copy payload/chunks and application metadata, and exclude link-local Busy and
feature metadata. They do not implement packed-ndarray format admission,
acquisition freshness, or held-state policy. Those remain explicit hold-node
checks. Extract a shared internal transfer utility only once the second caller
has demonstrated the exact common contract; do not build a generic storage or
policy framework first. Do not reuse the ring/lease machinery for a node whose
process state has one owner.

If a real slow device must remain a driver in another scheduling component,
the existing queue can be used unchanged as the explicit inter-component
handoff: slow source -> queue capture; separately paced queue playback -> hold
node -> fast composite. The queue's output still advertises 100 Hz; the hold
node performs the 1000 Hz transition. This costs extra endpoints but preserves
the queue's established purpose. It is a conditional deployment extension after
the simpler follower arrangement is tested, not a reason to embed hold policy
in the queue or assume one linked node can follow two independent drivers.

For a payload of B bytes, prepared copy storage moves approximately
`100 * B + 1000 * B` payload bytes per second through explicit copy operations
at the example rates, excluding memory read/write amplification and the
numerical backend's own transfers. The private-cache alternative may need only
the slow update copy. Measure B, memory locality, callback cost, and tails; do
not claim either placement faster without that evidence. The SPA node adds an
ordinary scheduling node but needs no extra thread when placed on an existing
data loop. An extra synchronous node does not by itself imply an extra quantum;
ASYNC placement would change that latency contract.

Scope the first hold-node age limit to fast driver cycles or a declared graph
time bound. A one-input follower cannot directly count primary *sample arrivals*
on another node if requested empty cycles or missing primaries are allowed.
Accept this explicit cadence-based contract for the fixed-rate first fixture;
do not hide a primary-trigger dependency in the implementation. The composite
still requires the actual primary sample before publishing a command. Preserve
the held sample's Acquisition identity rather than synthesizing a new physical
acquisition every fast cycle; command metadata belongs to the actual primary.
Strict primary-arrival-based expiry would be a different requirement and could
justify a cadence input or private ingress later.

Define output Header behavior explicitly as well. Acquisition identity must
remain the slow physical identity, but Header sequence, presentation time, and
DISCONT describe a media-specific publication contract
(`spa/include/spa/buffer/meta.h:85-97`). Repeating the complete source Header
would repeat a source discontinuity flag on every held output; creating a new
Header sequence must not be mistaken for creating a new physical acquisition.
The existing queue correctly forwards Headers unchanged because it is a
transport. Its transfer helper alone cannot choose the hold node's policy.

## Findings

| ID | Severity | Confidence | Classification | Recommended disposition |
| --- | --- | --- | --- | --- |
| SMR-001 | High | High | Observed integration contradiction | Resolve external/internal rate mapping before latest/hold implementation |
| SMR-002 | High | High for contract gap; source-dependent runtime impact | Observed scheduling semantics; derived risk | Specify and test one driver/follower arrangement |
| SMR-003 | High | High | Observed identity-ordering gap | Define authority transitions and replay rejection before exact join |
| SMR-004 | High | High | Observed output-gated admission | Decide starvation semantics before claiming backend parity |
| SMR-005 | High | High for missing lifecycle hook | Observed pre-existing gap; derived extension risk | Prefer prepared copying initially, or close lease-removal lifecycle first |
| SMR-006 | Medium | High | Observed parity differences and missing rules | Write one small behavior table and common event traces |
| SMR-007 | High | High for backpressure; deployment reachability untested | Derived observer/actuator coupling | Omit observer from first fixture or add an explicit isolation boundary |
| SMR-008 | Medium | High | Observed latency infrastructure; incomplete performance contract | Reuse standard reporting; separate held age and execution timing |
| SMR-009 | Medium | High | Observed queue/hold contract mismatch | Use a separate simple SPA rate-transition node; no queue policy extension |

### SMR-001 — The native format boundary cannot currently publish unequal input rates

Evidence: proposal lines 199-205 and 255-259;
`spa/plugins/filter-graph/filter-graph-ndarray.c:1205-1247`;
`doc/dox/internals/filter-graph-ndarray.md:257-266`;
`src/modules/module-ndarray-filter-chain.c:222-242`, `:806-822`.

**Observed:** FGN rejects different specified rates among external data inputs
and among a numerical node's data inputs. This is also an explicit maintained
FGN requirement. The module publishes the formats returned by FGN and rejects
negotiated formats whose rates differ from them. Julia's adapter separately
overrides all latest/hold input and output port rates with the primary carrier
rate (`../JuliaFilterGraph.jl/julia/FilterGraphPipeWire/src/graph_node.jl:243-296`).

**Derived:** a native graph described directly with 1000 Hz and 100 Hz external
FGN input formats fails construction. Keeping both graph inputs at 1000 Hz
while only changing PipeWire's advertised slow input rate fails the module's
current format validation. Adding a private cache before `graph_process()`
does not resolve this contract on its own.

Recommendation: explicitly distinguish the external sample arrival format from
the synchronous values supplied on admitted graph calls. Keep this mapping
local to the one latest/hold boundary and preserve shape, element, layout, and
schema validation. State where the held port's external rate is configured and
which rate the numerical graph sees. A rate-neutral internal descriptor is
possible only where already supported and scientifically appropriate; globally
relaxing FGN rate validation would silently change the existing contract.

Required validation: construct and link a real 100 Hz source to the held port,
inspect both negotiated external formats, then demonstrate successful native
processing with the chosen internal representation. Add negative link-format
checks so this change does not admit unrelated unequal-rate links.

### SMR-002 — Independent admission does not create independent scheduling

Evidence: proposal lines 65-75 and 207-210;
`src/pipewire/ndarray-filter.h:48-60`;
`src/pipewire/ndarray-filter.c:985-1043`;
`src/pipewire/filter.c:1013-1051`, `:2156-2170`;
`src/pipewire/impl-node.c:1319-1326`, `:2180-2193`;
`doc/dox/internals/scheduling.md:153-186`, `:341-375`, `:410-456`.

**Observed:** the helper's independent-input flag decides whether to invoke its
owner after PipeWire has scheduled the filter. `pw_filter_trigger_process()` on
an ordinary follower sends a RequestProcess event to the selected driver.
`node.trigger=true` adds a dependency; it does not mean “run whenever any input
arrives.” The scheduler assigns one graph rate and quantum to the component.
Publishing an ndarray format rate is not an instruction to wake that port at
that frequency. ASYNC is a separate dependency and buffer-I/O choice; driver
candidates have graph ASYNC disabled even while following another driver.

**Derived:** independently arriving hardware inputs do not automatically yield
independent composite callbacks. A slow follower must return promptly with a
new sample or no sample in a driver cycle, or participate in an explicitly
supported request/ASYNC arrangement. Otherwise a slow source can delay the fast
dependency path, or its data may wait for the next primary cycle. Actual
behavior of the selected camera source is a **hypothesis** until inspected or
tested; no source-specific deadlock is claimed here.

Recommendation: make the primary-driven cycle with a nonblocking slow follower
the first supported arrangement. Specify no-arrival signaling, whether held-only
callbacks can occur, and the effect of driver stop or migration. Do not add
explicit triggering or ASYNC merely because rates differ. A request-driven
deployment must name a driver that services RequestProcess. Verify driver
identity from the live graph; setting several nodes to `node.driver=true` does
not grant them independent cadence ownership after linking them.

Required validation: a live 1000/100 Hz fixture with driver-ID inspection,
callback counts, missing slow samples, phase offsets, primary stop/restart, and
competing driver candidates. Record whether extra requested cycles occur and
whether they can publish a command without a primary arrival.

### SMR-003 — “Newer acquisition identity” needs a defined ordering and retirement rule

Evidence: proposal lines 176-192 and 290-292;
`doc/dox/internals/acquisition-metadata.md:43-76`;
`spa/include/spa/buffer/meta.h:505-517`;
`../JuliaFilterGraph.jl/julia/JuliaFilterGraph/src/synchronized_inputs.jl:162-219`.

**Observed:** the identity domain is opaque; generation is assigned by its
authority. The existing identity helper checks complete equality, not an
ordering across domains or generations. The Julia owner remembers the last
closed sequence, including after its open window is cleared. Its Header
discontinuity path can reopen a sequence namespace; that behavior cannot simply
be carried over to authority-qualified identities.

**Derived:** lexicographic tuple comparison would invent an ordering between
unrelated authorities. Forgetting the last retired identity permits replay:
`A(K), B(K), A(K), B(K)` can complete twice after clearing the first window.
Treating any DISCONT header as permission to reset sequence ordering contradicts
the acquisition authority's generation contract.

Recommendation: pin the admitted domain and generation for the active session;
compare sequences only inside that namespace. Reject mismatched authority
values until a deliberate reconfiguration/reset installs the new namespace.
Retain a fixed retirement watermark as well as the one open window. Require
valid acquisition metadata with `IDENTITY_VALID`, not merely a structurally
valid record. No additional acquisition queue is needed.

Required validation: repeated complete keys after publication/expiry, wrong
domain with larger numeric bytes, wrong generation, sequence reset with DISCONT,
generation transition through lifecycle, and mixed old/new-generation arrivals.
Acquire-window ordering must also be deterministic when several ports are
available in one callback; see SMR-006.

### SMR-004 — Output starvation currently prevents slow input admission

Evidence: proposal lines 146-158 and 304-315;
`src/pipewire/ndarray-filter.c:1028-1043`, `:1073-1096`;
`src/modules/module-ndarray-filter-chain.c:525-537`, `:577`.

**Observed:** both adapters require an output buffer for every output before
calling the numerical owner. In the standalone helper's normal drain-to-latest
mode, unavailable outputs cause input recycling without an owner callback.
Thus Julia cannot copy a held-only arrival when one output pool is exhausted.
FIFO changes the behavior but also retains ordered input work, contrary to the
proposed latest-value policy.

**Derived:** the proposed rule “on held arrival, replace retained state” does not
hold under current output starvation. Copying versus leasing alone cannot give
the two backends identical admission traces. Counting age only in owner
callbacks also ignores primary arrivals that the helper discards before entry.

Recommendation: choose one documented behavior. The smallest compatibility
choice is to define arrivals at the owner-admission boundary and explicitly
count/drop transport arrivals that output starvation prevents admitting. If
the required behavior is to update held state regardless of output capacity,
the adapter needs a bounded input-admission path before output acquisition; this
is an explicit helper change, not a Julia policy fix. Do not solve it by
introducing a backlog of obsolete held samples.

Required validation: exhaust outputs, deliver newer held data and primaries,
restore outputs, and check the exact retained identity, age, counters, and first
published command in both backends. Decide whether a completed join with no
output capacity is dropped or retained; test progress after capacity returns
without relying on another hardware acquisition.

### SMR-005 — Cross-callback buffer leases require removal handling, not only stop/reset

Evidence: proposal lines 153-168;
`src/modules/module-ndarray-filter-chain.c:473-490`, `:781-787`, `:823-831`;
`src/pipewire/filter.c:739-765`;
`src/pipewire/ndarray-filter.c:1546-1627`;
prior finding `docs/REVIEW.md:82-148` (AO-REV-001).

**Observed:** the native module has no remove_buffer callback. PipeWire emits
that callback before unmapping pool memory. The standalone helper already has
specific retained frame-input/output invalidation handling. The earlier review
documents the wider missing lifetime transition; this review confirms the
module hook is still absent rather than claiming a new executed reproduction.

**Derived:** adding retained inputs to the module without covering pool removal,
renegotiation, and unlink extends the existing stale-borrow risk. Clearing on
deactivation alone does not establish the full removal protocol. A minimum pool
size also has to cover source ownership, transit, replacement, and any fan-out
leases; “one retained plus producer progress” is not a negotiated proof.

Recommendation: prepared copy storage is the simpler first native implementation
unless measured payload-copy cost rules it out. It matches Julia's ownership
without adding long-lived input leases. If native leasing is necessary, make
removal-before-unmap handling, data-loop serialization, and pool-count
validation prerequisites. Preserve immutable input access. Distinguish one
persistent retained slot from the transient newly dequeued replacement.

Required validation: remove/reconfigure a held-input pool, stop/reset/restart,
replace while retained, exercise the minimum accepted pool size, and verify no
stale access or invalid recycling under sanitizers. Copying avoids the new
retained-input lease but does not fix the module's existing retained-output and
parameter lifetime issues; those remain separate baseline prerequisites.

### SMR-006 — Backend parity requires several small decisions beyond acquisition metadata

Evidence: proposal lines 146-168, 261-268, and 304-308;
`../JuliaFilterGraph.jl/julia/JuliaFilterGraph/src/latest_hold.jl:67-110`,
`:169-205`, `:271-309`;
`../JuliaFilterGraph.jl/julia/FilterGraphPipeWire/src/graph_node.jl:1148-1177`,
`:1198-1265`, `:1016-1038`, `:1331-1334`, `:773-788`;
`../PipeWireAO.jl/src/ndarray_filter.jl:143-184`, `:209-223`.

**Observed:** Julia requires an integer primary/held ratio and derives its age
limit from that ratio. It ingests the held input before the primary when both
arrive in one callback. A corrupt held sample invalidates the previous held
value. Its age counts primary owner invocations, and a corrupt primary consumes
an age step. Deactivation only clears `owner.active`; reactivation after initial
warmup does not clear the prepared join. The generic synchronized-input adapter
consumes available ports in declaration order. Its numerical metadata projection
and finalization handle Header metadata, not Acquisition metadata.

**Derived:** the document's configurable age limit and rejection language do not
yet describe Julia's behavior. Callback-batch ordering changes which held value
a primary uses and can change join outcomes: with A(K) already retained, a batch
containing A(K+1) and B(K) can either expire K or complete K depending on order.
The multi-actuator promise also needs explicit output Acquisition assignment;
merely exposing input acquisition fields is insufficient.

Recommendation: specify one small state-transition table covering simultaneous
arrivals, corruption versus preservation of the last valid sample, stale and
duplicate primaries, age inclusivity and skipped acquisitions, ratio acceptance,
deactivation/reset, and publication metadata. For latest/hold, label command
outputs with the primary acquisition; for exact join, use the admitted common
identity. Clearing ingress on stop must not accidentally reset numerical
integrator state or active parameters if existing stop semantics preserve them.

Parity can use the existing Julia owner plus a small native implementation and
common immutable event traces. That is two implementations of a fixed contract,
not two schedulers. If a single semantic implementation is an explicit product
requirement, put these two fixed policies once at the common C PipeWire boundary
and keep Julia in ordinary synchronous mode on that path. That option requires
adapter API work and should not be disguised as an already shared facility.
Do not run a C ingress owner and a Julia join owner on the same sample path.

Required validation: compare publications, acquisition keys, and counters for
the same batched event stream, then repeat across stop/start without reset and
explicit reset. Include branches that do not run during initial warmup when
checking allocation and exception containment.

### SMR-007 — Ordinary fan-out does not isolate the optional recorder

Evidence: proposal lines 89-99 and 270-280;
`src/modules/module-ndarray-filter-chain.c:525-535`;
`src/pipewire/ndarray-filter.c:1028-1043`;
`doc/dox/internals/row-block-ndarrays.md:275-287`.

**Observed:** every exposed output requires an available output buffer before
either current adapter processes. Existing row-block guidance explicitly calls
for bounded observer isolation and distinguishes lease storage from copying.

**Derived:** a telemetry output that cannot obtain a buffer can prevent actuator
commands even if it is called optional in the diagram. Sharing a normal output
through fan-out also does not prove that a slow observer releases upstream
buffers promptly. ASYNC scheduling by itself does not provide an independent
payload pool. Exact runtime reachability depends on the selected link/buffer
setup and is not tested here.

Recommendation: omit frame telemetry from the minimal fixture, or explicitly
use a bounded observer-owned copy/drop boundary. Keep low-rate counter snapshots
available independently. Declare whether all actuator outputs are required as a
set and what the session does on one sink's loss; do not imply atomic physical
application. This is a topology/capacity decision, not another ingress policy.

Required validation: stall and disconnect the recorder while the actuator path
continues; inspect upstream pool pressure. Separately starve one required
actuator output and verify the declared stop/drop behavior and metadata on
recovery.

### SMR-008 — Standard latency reporting already exists and is not sample freshness

Evidence: proposal lines 212-218 and 293-300;
`src/pipewire/filter.c:769-825`;
`doc/dox/internals/latency.dox:40-84`, `:103-115`;
`doc/dox/internals/acquisition-metadata.md:169-176`.

**Observed:** pw_filter already propagates combined upstream/downstream latency
and adds ProcessLatency unless custom handling is selected. SPA latency units
refer to graph quantum, graph sample rate, or nanoseconds. A single per-node
ProcessLatency fits paths with similar contributions; held inputs and primary
inputs have different age histories. Acquisition-driven expiry releases a
window on a newer acquisition but has no finite wall-clock bound during silence.

Recommendation: reuse existing PipeWire latency propagation and report measured
or conservatively specified local contributions for the chosen deployment.
Prefer nanoseconds where per-port sample rates would make conversion ambiguous.
Define a conservative aggregate if that is sufficient; do not introduce a new
per-port latency propagation engine without a consumer requiring it. Report
held age, join waiting, execution duration, and source health separately when
they answer different questions. Low-rate snapshots need a real synchronization
protocol; a sole data-loop writer does not make unsynchronized C readers safe.

Do not describe acquisition-driven expiry as a wall-clock deadline. State its
indefinite-silence behavior and the concrete lifecycle/health mechanism that
releases resources. If a deployment requires timed failure detection, a bounded
existing control/watchdog path can stop the session without scheduling frames.

Required validation: verify nonzero published latency where known and ordinary
propagation; record event-to-callback and callback-to-command distributions under
idle, nominal, burst, starvation, and recovery. Specify sizes, core/runtime
budget, loss semantics, and target tails before claiming deployment acceptance.

### SMR-009 — Reusing the queue as a hold converter changes its defining contract

Severity: Medium; confidence: High. **Observed** evidence and technical analysis
are in the placement comparison above. A bounded queue and a held-value
rate-transition node share buffer mechanics but differ in cardinality of
delivery, format-rate preservation, scheduling ownership, and retention after
delivery. The change is architectural, not a missing overflow option.

Recommended disposition: choose the separate single-node implementation for
the first 100/1000 Hz case. Keep the existing queue unchanged for observer
isolation and conditional inter-component handoff. Share only a proven small
transfer primitive if implementation exposes an actual second caller. The
separate-node baseline avoids the composite-private integration problem in
SMR-001, centralizes the policy differences in SMR-006, and keeps slow updates
independent of composite output availability. It does not automatically solve
source scheduling, command-output starvation, freshness, or recorder isolation.

Required validation: with one accepted input and ten fast graph cycles,
demonstrate repeated immutable output values, identical physical acquisition
identity, the declared expiry boundary, and no output before the first valid
input. Contrast with a capacity-one queue delivering its sample once and then
being empty. Test slow arrival while hold output is starved, ensure the cache
still replaces/ages as specified, and recover with the latest eligible sample.
Link the same node to native FGN and ordinary Julia graphs; do not enable either
backend's private latest/hold owner on this path. Test pool removal, pause and
driver replacement, exact rate negotiation, and bounded copying/allocation.

## Source-claim corrections and simplification

The proposal correctly describes the native module's drain-to-latest data input
behavior, absence of retained data-input values, Julia's Header-only join key,
the Julia carrier-rate override, and RTC's single session-rate assumption.
Qualify “returns all input buffers” as frame-data inputs: Parameter Ports have
their own retained worker handoff. Qualify FIFO as helper-level ordered admission
through bounded pools, not a general end-to-end lossless guarantee.

`pipewireao-rtc/src/config.rs:151-158` contains name, direction, Parameter role,
element type, shape, and schema. It does not contain a layout field. The phrase
“layout role” in the draft should identify the actual separate constraints or
be corrected. Its delegated `filter.graph` ownership claim is supported by
`:160-173` and `../pipewireao-rtc/docs/architecture.md:31-41`, `:234-244`.

The C callback structure already contains Acquisition by value
(`src/pipewire/ndarray-filter.h:161-170`), and PipeWireAO.jl can already copy it
with `propagate_metadata!`. The missing piece is a supported Julia inspection/
construction path and the graph owner's acquisition-aware policy and output
propagation; no new core metadata record is necessary.

Recommended sequence:

1. Implement and document one latest/hold SPA node with prepared storage, exact
   external rate transition, cadence-based expiry, required metadata, lifecycle,
   and essential counters. Add RTC per-port rates where needed to declare this
   topology. Keep numerical backends in ordinary synchronous mode.
2. Qualify one primary-driven live fixture with native FGN and Julia substitution,
   then measure realistic payload-copy cost and timing. Reuse standard latency
   reporting. Use the existing queue unchanged if recorder isolation is in scope.

Defer exact acquisition joining entirely from these deliveries. It is a separate
multi-camera correctness feature and is not needed to repeat a slow value at a
fast cadence. Do not retain its parser, mode enum, join counters, or per-backend
owner work in the initial implementation merely because the draft listed two
modes. Its existing normative acquisition requirements remain applicable when
a selected consumer needs it. If that later consumer needs matching, first
determine whether equality validation of a same-cycle complete set suffices;
otherwise a one-window join with SMR-003 and output-starvation rules is a bounded
next design. Timestamp matching and wider windows remain deferred.

Remove the implied first-delivery obligation to integrate every listed kind of
node or transport. Preserve the guardrails against algorithm queues, dynamic
policies, graph rollback, automatic scientific conversion, and a second RTC
graph language. Prefer a concrete latest/hold node contract to an extensible
ingress framework in anticipation of deferred modes. Sparse
Parameter Ports remain appropriate for calibration/configuration state; a truth
sensor measurement should not be reclassified as a Parameter merely to reuse
its worker, because that changes admission and preparation semantics.

## Verification performed and remaining evidence

Read-only source and document comparison covered the proposal, both native
adapters, FGN graph rate and process rules, PipeWire callback/trigger/latency
paths, the acquisition ABI/contract, existing retention review, Julia owners
and adapter, PipeWireAO callback metadata, and RTC configuration boundaries.
The follow-up comparison also inspected the external plugin project's queue
contract, both endpoint process callbacks, format and grouping setup, ring API,
and buffer-transfer implementation. No queue implementation changes were made.

Executed on existing build artifacts:

```text
meson test -C build --no-rebuild spa-filter-graph-ndarray-c \
  pw-test-ndarray-filter-admission pw-test-ndarray-filter-publication \
  --print-errorlogs

3 passed; 0 failed; 0 skipped.
```

These baseline tests establish that the existing selected graph/admission/
publication suites run. They do not verify the unimplemented proposal, establish
fail-before/pass-after evidence for a production fix, or qualify current source
against rebuilt binaries. No live mixed-rate graph, hardware camera, actuator,
accelerator transfer, multi-driver migration, latency distribution, or sanitizer
experiment was run for this review. Source-established gaps and derived risks
remain as individually classified above, pending primary-agent adjudication.

## Verification of revised proposal

Verified on 2026-09-13 against the 508-line revision of
`doc/dox/internals/simple-multirate-ndarrays.md`, SHA-256
`21ae764e9a342e58c2bc0506d27359d602b6e8fb00cc2413fcc32f1114e0f178`.
PipeWire remains at `ea9d90e27f1d4d6405f3466db57831512c7a0dc1` on
`master` in the worktree recorded above. This section supersedes the interim
acceptance of the 396-line revision and verifies the later independent
SMR-FINAL-001 through SMR-FINAL-007 findings. Earlier SMR-001 through SMR-009
findings remain investigative history, not current dispositions.

Recommendation: **approve the architecture and bounded implementation plan**.
All seven later findings are resolved in the design. Their implementation and
qualification remain open. In particular, the native buffer lifetime defect
is still present in current source and is now an explicit first delivery
prerequisite. No architectural blocker remains after the RTC lifecycle
clarification incorporated into this exact revision.

**Observed** means inspected source or the fingerprinted document; **derived**
conclusions concern feasibility and consequences of those facts. Live
scheduling, recovery, timing, and hardware behavior remain unverified. No
source-specific deadlock or timing bound is inferred from this review.

| Finding | Final design disposition and proposal evidence | Required implementation evidence |
| --- | --- | --- |
| SMR-FINAL-001: native output lifetime | Resolved in scope: lines 133-138 and 393-396 require the native retained-output removal repair | Fail-before/pass-after removal while a required input is absent; serialized invalidation before unmapping; sanitizer coverage where available |
| SMR-FINAL-002: cadence and advertised rate | Resolved: lines 206-224 require cycle-counter aging, one publication per cycle, exact rational period, NO_RATE rejection, and incompatible-cadence suppression | Overflow-safe arithmetic; skipped/duplicate callbacks, wrap, regression, driver change, equivalent clock representation, and recovery with a new valid sample |
| SMR-FINAL-003: no-buffer cycles | Resolved: lines 76-85 distinguish no new buffer from an empty HAVE_DATA frame | Prompt follower completion with ordinary ownership/recycling; nine nominal empty cycles preserve the slot without renewing age |
| SMR-FINAL-004: RTC realization and source | Resolved: lines 337-355 admit only NdarrayLatestHold and distinguish its SPA lifecycle; lines 226-230 disclaim a silence alarm; lines 409-413 require a qualified identity-bearing source | Factory/port/driver validation, session and execution-group lifecycle, and actual source behavior; no implicit FITS Acquisition support |
| SMR-FINAL-005: watermark and precedence | Resolved: lines 175-198 and 264-270 separate payload validity from identity history and define corruption, DISCONT, and lifecycle precedence | Duplicate after expiry/reset, duplicate-plus-corrupt, missing-identity-plus-DISCONT, lower sequence, and valid namespace transition |
| SMR-FINAL-006: logical latency | Resolved: lines 371-389 require bidirectional propagation, zero initial logical ProcessLatency, and separate runtime/freshness measurements | Verify propagation and any declared logical delay; do not report callback duration as signal delay |
| SMR-FINAL-007: command metadata source | Resolved: lines 250-260 and 318-323 preserve native declared provenance and specify Julia Acquisition copying before Header finalization | Final command Acquisition follows the declared main-WFS path in both backends; algorithm Header survives; missing records do not leave stale metadata |

The separate copied node remains the smallest shared boundary under the stated
constraint. **Observed:** FGN validates equal specified external data-input
rates at `spa/plugins/filter-graph/filter-graph-ndarray.c:1205-1247`.
PipeWire completes ordinary follower dependencies without requiring HAVE_DATA
(`src/pipewire/impl-node.c:246-255`, `:1699-1705`) and increments
`clock.cycle` when starting a graph cycle (`:2451`). **Derived:** a slow
source can complete without a buffer, then the hold node can publish retained
state before the ordinary composite runs. No extra timer, request cycle, or
numerical hold owner is necessary. The no-buffer rule does not authorize
overwriting an outstanding HAVE_DATA publication; standard ownership and
return processing still apply (`spa/include/spa/node/io.h:47-69`).

The cadence equation is correct: for positive valid fractions, cycle period
is `clock.duration * clock.rate.num / clock.rate.denom` seconds, compared with
`output_rate.denom / output_rate.num`. Units are defined at
`spa/include/spa/node/io.h:169-190`. An equivalent clock representation may
remain compatible; driver changes and discontinuities invalidate payload.
Implementation must bound modular-counter assumptions, preserve the identity
watermark independently, and release offered inputs on suppressed-output
paths. These are bounded arithmetic/ownership checks, not another scheduler.

**Observed:** the native module retains outputs at
`src/modules/module-ndarray-filter-chain.c:525-534` but lacks a removal callback
at `:781-787`; `src/pipewire/filter.c:739-765` removes and unmaps the pool.
The separate helper already invalidates retained frame inputs and outputs at
`src/pipewire/ndarray-filter.c:1546-1636`. Its current response fails closed
with `-EPIPE`, which does not establish transparent pool replacement. The
fixture must distinguish safe failure and lifecycle recovery from automatic
continuation. The native repair remains required despite copied hold storage.
Sparse Parameter Port lifetime issues outside the selected fixture remain
separate baseline work, not silently repaired by this plan.

The Rust additions match source: the common node I/O callback rejects all IDs
(`../pipewireao-spa-plugins-core/crates/pipewireao-spa-node/src/factory.rs:570-579`),
and its `src/pod.rs:44-88` advertises Header without Acquisition or Latency.
Position delivery, version-feature negotiation, metadata copying, and ordinary
latency handling are implementation work. Validate actual identity fields,
not merely an empty structurally valid Acquisition record, and detect
corruption in both Header and chunk flags. ACQ-ID-001/002 remain the source
authority contract (`doc/dox/internals/acquisition-metadata.md:43-72`). A valid
single-source namespace transition does not establish an ordering or arbitrary
replay protection across namespaces. The stricter exact join remains deferred.

The RTC lifecycle clarification closes an additional prerequisite exposed in
this pass. **Observed:** `../pipewireao-rtc/src/live.rs:1088-1197` expects
numerical run-control, reset-control, and scientific property snapshots;
`../pipewireao-spa-plugins-core/crates/pipewireao-spa-node/src/factory.rs:589-611`
supports Start/Pause. The revision excludes the hold factory from numerical
discovery and routes serialized SPA commands, including Pause before reset
and conditional Start afterward. This fits a small factory-specific dispatch
branch. Verify command completion and data-loop serialization; posting a
command alone is not completion. The source fixture remains necessary: current
FITS publication at
`../pipewireao-spa-plugin-fits/spa/plugins/fits/source.c:1265-1284` supplies
Header without Acquisition.

Native declarations already select and copy metadata sources
(`../calculon-algorithms/crates/calculon-fgn/src/algorithm.rs:1486-1503`).
`../PipeWireAO.jl/src/ndarray_filter.jl:207-224` copies Header and Acquisition
together; Julia graph finalization currently writes Header
(`../JuliaFilterGraph.jl/julia/FilterGraphPipeWire/src/graph_node.jl:773-794`).
Copying first and finalizing Header afterward preserves both meanings for
available outputs. Preparation must resolve output provenance through the
graph. Test missing Header as well as missing Acquisition so copied validity
bits do not override an absent algorithm-defined record. No numerical
execution or metadata ABI change is needed for the selected synchronous path.

The queue's separately paced endpoints retain their existing contract
(`../pipewireao-spa-plugins-core/docs/queue.md:140-149`). Conditional placement
at proposal lines 298-303 does not make the queue a hold converter. Validate
capacity, overflow, metadata negotiation, and driver placement if that later
variant is selected; no queue machinery belongs in the initial hold node.
Ordinary local exported nodes preserve PipeWire semantics but add placement-
dependent wakeup costs. The transport boundary and local-admission age limit
at lines 361-367 correctly avoid cross-host carriage or acquisition-age claims.

The latency treatment agrees with `spa/include/spa/param/latency.h:65-75`.
The performance profile at lines 456-466 requires payload sizes, load/bursts,
CPU/NUMA resources, allowed loss, and latency targets before qualification.
The copy estimate counts payload bytes passed through copies, not physical
memory traffic. No latency or zero-allocation result is established until the
source, hold node, numerical backend/runtime, output pool, and recovery paths
are measured together. Shared policy reduces parity work without removing
those verification obligations.

This pass changed only this final verification section. The earlier report
prefix was preserved byte-for-byte; the design fingerprint remained unchanged.
`git diff --check`, explicit whitespace/final-newline checks on both untracked
documents, and unique-section/fingerprint checks passed. No runtime tests were
rerun; earlier baseline-test limitations still apply. The plan is approved for
implementation. Native remediation, SPA/RTC/Julia changes, live parity/recovery
evidence, performance qualification, and physical-device validation remain
outstanding.

## Implementation remediation verification — 2026-09-13

This independent pass reviews the shared, uncommitted implementation, not only
the approved design. The base revisions are PipeWire
`ea9d90e27f1d4d6405f3466db57831512c7a0dc1`, Rust plugins
`ac82518cc876cb6fce8baf55fc6875f8fba0f71f`, RTC
`17365a4250dd0a718dc45fbd7985d47afd1374f4`, and JuliaFilterGraph
`54177fa887451ea9386f07bede18e9942a58fe76`. These sibling worktrees contain
pre-existing changes. A separate review worktree would omit the implementation
under review; this pass made no production edits or commits. The hot-path and
code-quality review skills were applied to ownership, bounded work, allocation,
API scope, and repository consistency.

### Disposition of implementation findings

Severity describes the original finding. Confidence is high unless stated.
Source-established defects are distinguished from observed test failures and
from optional improvements.

| ID | Severity and category | Final source verification and evidence | Disposition |
| --- | --- | --- | --- |
| SMR-IMPL-001 | High; observed integration defect | `crates/ndarray-spa-plugin/src/config.rs:7` now restricts unknown-key rejection to the `api.ndarray.` namespace, accepting ordinary host properties. Before remediation, adding only `node.name`, `factory.name`, or `node.loop.name` to a valid direct-factory dictionary returned `-EINVAL`; the exact seven application keys succeeded. The direct test and actual `spa-node-factory` test now pass with host properties. | Fixed |
| SMR-IMPL-002 | High; source-established factory-identity defect | RTC `src/live.rs:create_owned_latest_hold` obtains `factory.name` from the created node's NodeInfo listener, preserves it across partial updates, waits for a roundtrip, and validates the fixed factory. It no longer expects this property in the registry global. | Fixed |
| SMR-IMPL-003 | High; source-established corruption defect | `InputFrame::chunk_corrupted` exposes chunk flags; `latest_hold.rs:admit_input` tests chunk and Header corruption before identity ordering. Direct tests exercise advancing and duplicate corrupted chunks, payload invalidation, retained watermark, and discontinuous recovery. | Fixed |
| SMR-IMPL-004 | High; source-established lifecycle race | The wrapper discovers host DataLoop support and serializes Start/Pause with `spa_loop_locked`; a pending-command flag prevents processing from continually reclaiming the state gate. The overlap test exercises a blocked process callback, Props observation, and both lifecycle commands. The no-DataLoop direct host must serialize its callbacks itself. | Fixed for the ordinary PipeWire host contract |
| SMR-IMPL-005 | High; source-established Rust aliasing defect | `instance_ref` produces shared Handle references, mutable state stays inside the acquired `UnsafeCell` gate, listener hooks have separately documented main-loop ownership, and node/port notification snapshots own their parameter arrays. Atomic observers do not claim processing state. This removes overlapping whole-Handle `&mut` references and escaped references into mutable parameter storage. | Fixed by source ownership analysis; overlap test is supporting evidence, not a Rust memory-model proof |
| SMR-IMPL-006 | Medium; source-established discontinuity defect | An observed-cycle key and `observed_discontinuity` ensure that repeated callbacks in one DISCONT cycle do not invalidate the newly accepted replacement again. The direct C regression exercises the repeated callback and subsequent ordinary hold. | Fixed |
| SMR-IMPL-007 | Medium; source-established counter defect | Observed-cycle accounting is independent of admission of a compatible driver. The focused Rust regression checks incompatible cadence and repeated discontinuity callbacks without counting the same observed cycle twice. | Fixed |
| SMR-IMPL-008 | Low; accepted simplification | Acquisition validation calls the maintained SPA validator, with an explicit supported-version check. Sole-writer counters use relaxed load plus saturating store instead of a retrying read-modify-write loop; the saturation test passes. | Implemented; no separate metadata validator or counter retry loop remains |
| SMR-IMPL-009 | Medium; contract overclaim, not demonstrated binary substitution | RTC preflights the maintained artifact and requests the fixed library/factory name. The documented trusted core `context.spa-libs` mapping remains authoritative. The isolated fixture controls this mapping and search path; arbitrary-core binary fingerprint enforcement is not provided. | Closed by narrowing the claim; stronger external-core enforcement is outside this delivery |
| SMR-IMPL-010 | Medium; source-established invalid raw-slice construction | The final pass found that an empty dictionary could reach Rust `from_raw_parts(NULL, 0)`. Required lookups and validation now reject empty/null storage before constructing slices; optional lookup returns absence for an empty dictionary. The direct factory regression accepts the expected `-EINVAL` response. No fail-before crash was executed. | Fixed |
| SMR-IMPL-011 | Medium; observed validation weakness | The initial live oracle required only nonzero final commands. The final fixture checks contiguous command identities through 100, with the first command no later than 11; this requires all 90 identities in the designated steady-state comparison window. Both backends retain ordinary drain-to-latest admission. | Oracle strengthened and verified; identical startup counts remain unclaimed |

The lifecycle ownership distinction is now documented: `spa_loop_locked`
serializes with the data loop, but can execute the operation on the calling
control thread while holding the loop lock. It does not guarantee that every
lifecycle write physically executes on the data-loop thread. Processing itself
remains a single writer, with lifecycle mutation excluded from concurrent
processing. The plan also now distinguishes a structurally valid Acquisition
record without `IDENTITY_VALID` from malformed metadata, matching admission
precedence in the implementation.

### Architecture and adjacent implementation

The shared copied hold node remains the smallest idiomatic PipeWire solution
for the agreed scope. Its process path reads the driver's Position, validates
and copies at most one offered ndarray, ages one retained payload, and publishes
at most once per observed cycle. Exact rational cadence comparison and unsigned
cycle differences match the documented units and half-range assumption. Payload
validity is independent of the accepted Acquisition watermark. There is no new
FIFO, ring, timer, worker, or private scheduler in this node. Standard SPA
commands, metadata, negotiated buffers, port latency, and existing driver
scheduling remain the integration boundaries.

The generic Rust additions affect existing factories as well as latest/hold.
They expose Position and Acquisition when requested and ordinary Latency plus
zero logical ProcessLatency. Opposite-direction latency ranges are combined
and forwarded with parameter serial notifications. This is standard SPA
propagation, not a runtime latency estimator; zero does not claim zero callback
time, zero hold age, or zero buffering under starvation. Existing ndarray
transform/frame-assembly regression tests pass. Generic port/format enumeration
and latency mutation still use the state try-gate, unlike the hold's atomic
Props observer; concurrent control traffic can return `-EBUSY` or cause process
to return NEED_DATA. No live loss from this case was demonstrated in this pass,
so noninterference under such traffic remains a qualification gap.

Julia prepares the graph's output-to-external-input provenance mapping once,
caches a concrete tuple in the owner, copies Header/Acquisition through the
existing PipeWireAO helper for available outputs, then applies graph-defined
Header finalization. The resolver was factored from the existing captured-graph
implementation; it does not introduce another join or retention policy.

The native filter-chain removal callback synchronizes with the data loop and
clears a matching retained output before pool unmapping. Its direct regression
passes. This establishes the narrow stale-pointer repair, not transparent
end-to-end pool replacement or successful numerical-owner recovery.

### Verification evidence and remaining qualification

This reviewer independently ran the following checks successfully:

- Rust-plugin Meson tests `spa-ndarray-latest-hold`,
  `spa-ndarray-latest-hold-factory`, and `spa-ndarray-transforms` (3/3), then
  repeated both hold tests after the empty-dictionary fix (2/2).
- PipeWire `pw-test-ndarray-filter-chain-buffer-removal` (1/1).
- Rust `pipewireao-spa-node` test
  `process_props_and_lifecycle_overlap_without_busy_errors` (1/1), and
  `pipewireao-ndarray-spa-plugin` `latest_hold::tests` (5/5).
- The final focused native/Julia live fixture (1/1, 7.13 seconds), using the
  command below from the RTC worktree.
- `git diff --check` across the four worktrees.

```sh
PIPEWIREAO_RTC_LIVE_SCOPE=latest-hold \
PIPEWIREAO_SPA_PLUGINS_BUILD=/home/dgamroth/workspaces/codex/pipewire/pipewireao-spa-plugins-core/build \
PKG_CONFIG_PATH=/home/dgamroth/workspaces/codex/pipewire/pipewire/build/meson-uninstalled \
cargo test --features live --test live_private_core -- --ignored --nocapture
```

The earlier focused live fixture also passed independently, but its command
assertion required only nonzero output. That result does **not** establish
native/Julia availability parity. Strengthening the oracle exposed a mismatch:
the native branch delivered primary identities 1–100, while Julia delivered
6–100, despite 100 hold publications in both cases. These sequence observations
locate the deficit at startup rather than at the end of the stream; they do not
alone identify its cause. FIFO and explicit-precompile experiments did not
resolve it according to the primary agent and were removed; neither a
compilation cause nor a transport-coalescing cause is established here.

The final passing fixture uses ordinary drain-to-latest admission for both
backends, explicitly designates the first ten primary cycles as a startup
window, and requires every primary identity 11–100 with the correct payload,
Header, and Acquisition on each command branch. An arbitrary late singleton
cannot pass. The hold boundary remains exact: ten accepted slow acquisitions,
100 validated publications, zero rejected updates/protocol errors/starvation,
and suppression after expiry. The fixture also checks Position rate `1/1000`
and quantum 1, ordinary Dummy Driver use, no replay after Pause/Start, owned
node removal, and preservation of the external Julia owner on RTC unload.

This is evidence for the selected steady-state use case, not identical cold
startup availability, lossless admission, a maximum startup-time guarantee,
or completion of every live acceptance clause in the plan. The private-core
fixture only lowers the permitted minimum/floor quantum; its ordinary default
and maximum are unchanged, and the source explicitly requests quantum 1.

The primary agent supplied a warmed Julia callback regression reporting zero
allocations and a practical heaptrack capture at
`/tmp/pipewireao-latest-hold-final.heaptrack.zst` without a latest/hold-process
allocation stack. The latter is not an allocator-trap proof. Neither the unit
tests nor live functional counts establish worst-case execution time, latency
percentiles, jitter, overload behavior, or a payload-size crossover. Cross-process
pool replacement/recovery, live Reset and selective execution-group coverage,
and physical camera/actuator behavior remain separate qualification work.

Final recommendation: accept the remediated implementation as the bounded
development baseline. No confirmed source defect blocks that scope after this
verification. Do not promote it to full live-recovery, allocation, timing, or
hardware qualification, or claim identical native/Julia startup delivery. The
remaining gaps call for targeted measurements and lifecycle fixtures, not a
second hold policy, FIFO, scheduler, or speculative execution abstraction.

## Lease-only implementation addendum — 2026-09-14

The user superseded the copied-payload baseline: latest/hold must retain
PipeWire leases and must never copy ndarray payloads. This addendum records the
independent review of that implementation and supersedes copied-payload wording
elsewhere in this historical report.

| ID | Disposition | Evidence and remediation |
| --- | --- | --- |
| SMR-LEASE-001 | Confirmed, resolved | A borrowed input pointer cannot outlive input-pool withdrawal, and PipeWire peer teardown does not reliably honor `-EBUSY`. Output allocation now duplicates each negotiated MemFd and owns an independent read-only mapping. The lifecycle regression withdraws the input pool, closes and unmaps all upstream buffers, then reads the outstanding output successfully. |
| SMR-LEASE-002 | Confirmed, resolved | Aliasing the input chunk pointer would let output publication mutate input-owned metadata and would leave output metadata dangling. Output chunk, Header, and Acquisition storage now remains output-owned; only their bounded values are copied. |
| SMR-LEASE-003 | Confirmed, resolved | A copied source descriptor advertised writable/dynamic memory despite a read-only fixed mapping, and `mmap` alone does not reject an extent beyond EOF. Output aliases now advertise `READABLE|MAPPABLE`, clear `WRITABLE|DYNAMIC`, and validate `mapoffset + maxsize` against `fstat` before mapping. The C regression checks both contracts. |
| SMR-LEASE-004 | Confirmed, resolved | The synchronous no-callback return path has one I/O return slot. A three-input regression now holds A downstream, admits B, then returns A while C is offered. Admission detaches C and services older retired leases before retiring B; the bounded sweep returns A without a FIFO or unbounded scan. |
| SMR-LEASE-005 | Confirmed, resolved | Output allocation requires the matching input pool to exist. RTC realizes latest/hold ingress allocation dependencies upstream-first, including cascaded holds, and then preserves downstream-first ordering for remaining links. A focused ordering test and the native/Julia live fixture pass. |

The final Astra verification found no blocking defect in these remediations.
The repeated process path retains one current lease plus a fixed-width retired
bitset, copies only metadata, and performs no heap allocation, mapping, payload
copy, wait, worker dispatch, or unbounded work. Descriptor duplication,
`fstat`, and `mmap` occur only during buffer-pool negotiation.

Final verification after the lease findings:

- all plugin Meson tests: 18/18;
- Rust plugin workspace library tests: 35/35, plus workspace Clippy with only
  the maintained PipeWire-header unused-parameter warning;
- RTC live-feature unit tests: 6/6 and configuration tests: 22/22;
- focused native/Julia private-core latest/hold fixture: 1/1; and
- native ndarray filter-chain retained-output removal regression: 1/1.

Residual qualification is unchanged in kind: exercise cross-process live pool
replacement, measure mapping first-touch and tail latency, and require producers
not to truncate or rewrite backing still covered by an outstanding lease. A
duplicated descriptor preserves object lifetime; it cannot make a mutable or
subsequently truncated backing into an immutable snapshot.
