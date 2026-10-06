# PipeWireAO fork review

Review date: 2026-09-10. Disposition: retain the architecture; remediate the
buffer-lifetime defect and graph-admission defect before treating this revision
as a robust runtime baseline. The fork is a credible integration prototype,
not an established end-to-end real-time controller qualification.

## Review boundary and evidence

| Item | Value |
| --- | --- |
| Upstream comparison base | `origin/master`, `fff1bcf7e5399f3945c8ed5558870696dc23597a` |
| Reviewed fork tip | `82d948a2f02b4dc82903fd9334980d7705374ff7` |
| Merge base | Same as upstream comparison base |
| Review branch | `review/fork-vs-origin-20260910` |
| Review worktree | `/tmp/pipewire-fork-review-20260910` |
| Starting changes | None |
| Change scope | 187 commits; 165 changed files; 32,658 additions and 474 deletions |
| Review environment | Linux `6.12.57+deb13-amd64`, x86-64; GCC 14.2.0; Meson 1.7.0 |
| Production mutations | None; only this report and two investigative reproductions were added |

This was an independent source and architecture review against the exact local
upstream revision, including surrounding upstream implementations, fork history,
normative documentation, the traceability ledger, build definitions, and focused
test sources. Inspection concentrated on scheduling publication, ndarray
admission and retained storage, control/data handoffs, fixed-worker execution,
ABI and installation boundaries. It is not a line-by-line proof of every added
test or an audit of external camera SDKs and Calculon implementations.

The audit-code-slop and review-low-latency-design skills and their required
references were applied. The hotspot scan found 45 mechanical signals in five
major implementation files. Most long identifiers are normal namespaced C ABI
names; length and duplication were not treated as defects without consequences.
The relevant duplication consequence is adapter lifecycle drift in AO-REV-001.

Evidence terms used below:

- **Observed:** source, configuration, or executed probe directly establishes it.
- **Derived:** a consequence of those observations under stated preconditions.
- **Hypothesis:** a remaining scenario requiring a discriminating live test.
- **Speculation:** insufficiently supported; not promoted to a defect.

## Executive assessment

The present fork is substantially more PipeWire-idiomatic than its historical
custom transport experiment. The history shows the removal of camera plugins,
private RTC transport, progressively mutated public buffers, and a private RTC
loop. Current processing uses ordinary SPA buffers, PipeWire dependency
scheduling, an opt-in polling wait policy, and one composite filter node for the
internal ndarray graph. These are useful boundaries. Replacing the composite
graph with one public PipeWire node per numerical operation would reintroduce
scheduling and ownership costs without an established requirement.

Exact ndarray formats and semantic schemas, Acquisition V2 metadata, declared
conditional outputs, sparse Parameter Ports, failure-atomic property staging,
and a versioned C plugin boundary all have concrete consumers and documented
roles. Their existence is justified. Generic scientific state remains in
plugins; device and observatory policy is assigned to sibling repositories.
The fixed-worker executor has a clear synchronous borrow contract and a serial
default. None of these mechanisms should be deleted merely because the new
code is large or its C names are long.

The most consequential problem is incomplete lifetime handling when negotiated
buffers are removed while an adapter retains them. A second reproducible
problem is graph construction accepting malformed configuration prefixes. The
bounded retain-oldest mechanism is justified, but its public FIFO/lossless claim
is too strong and its live test does not exercise the backlog-to-RequestProcess
progress path it introduces. Current performance
documentation generally distinguishes microbenchmarks from system qualification
correctly; those qualifications must remain explicit.

| ID | Severity | Confidence | Category | Recommended disposition |
| --- | --- | --- | --- | --- |
| AO-REV-001 | High | High for missing lifetime transition; medium for live trigger reachability | Correctness/lifetime | Confirmed source defect with an executed lifecycle model; remediate and reproduce live |
| AO-REV-002 | Medium | High | Configuration admission | Confirmed by executed public-API probe; remediate |
| AO-REV-003 | Medium | High | Admission contract and validation gap | Rename/qualify the policy and add a discriminating live regression test |
| AO-REV-004 | Medium | High | Real-time acceptance gap | Keep deployment qualification open; no speculative tuning |
| AO-REV-005 | Low | High | Evidence maintenance | Retain and simplify the ledger; pin reproducible evidence |

## Findings

### AO-REV-001 — Removed buffers can remain borrowed by ndarray adapters

Affected code: `src/pipewire/ndarray-filter.c:786`, `:1546`, `:1593`;
`src/modules/module-ndarray-filter-chain.c:357`, `:473`, `:533`, `:781`;
upstream lifetime boundary `src/pipewire/filter.c:739`.

**Observed:** the standalone helper publishes `pending_parameter` and a
`parameter_view` containing a borrowed payload pointer to its worker. Its
`remove_buffer` hook explicitly excludes Parameter Ports from invalidation.
That worker can run after Pause: `deactivate()` clears `prepared`, while
`update_parameter()` tests `destroying` and `pending_parameter`, not
`prepared`. It can also hold a parameter for a later `-EBUSY` retry. The
underlying `pw_filter` calls `remove_buffer` immediately before unmapping a
mapped buffer, including during replacement through `port_use_buffers()` and
Format changes.

**Observed:** the filter-chain module has no `remove_buffer` callback. It retains
both pending parameter buffers and conditional output buffers across process
calls. Thus the new standalone input/output invalidation logic does not cover
the module's equivalent ownership. The module's control mutex serializes plugin
control callbacks but does not, by itself, cancel a queued parameter borrow or
clear a retained output when PipeWire removes its pool.

**Observed experiment:** [parameter-removal.c](review/parameter-removal.c)
uses the existing admission fixture, projects a valid mapped parameter through
the production validator, creates the pending state, and calls the production
remove hook. The result is:

```text
pending_after_remove=1 prepared_after_remove=1 error=0
worker_after_unmap_signal=11
```

The second line runs the actual `update_parameter()` function after modeling
the subsequent unmap in an isolated child with core dumps disabled. The callback
faults on its payload read. This demonstrates the missing invalidation and its
memory-access consequence. It is deliberately a lifecycle model, not a claim
that a live daemon unlink was reproduced. The fixture sets the filter pointer
to NULL to choose the hook's already-quiescent path, and the test establishes
pending state directly rather than exercising the worker queue.

**Derived:** pool replacement or unlink with a pending or in-flight parameter
can leave an invalid descriptor/payload reachable by the worker; retained module
outputs can similarly refer to removed or repurposed pool storage. This violates
the documented borrowed-buffer lifetime and can produce a crash, stale-data
processing, or invalid buffer recycling. Callback serialization at normal
graph stop does not cancel a borrow stored for a later worker attempt.

**Smallest coherent remediation:** define one removal protocol covering frame
inputs, conditional outputs, and parameter handoffs in both adapters. Stop new
admission under the data-loop ownership boundary; cancel unclaimed parameter
work; wait for any claimed callback before permitting its storage to be unmapped;
clear pending/completion/retry state; then fail the affected processing interval
or explicitly rebuild it. A data-loop lock alone cannot establish worker
quiescence. Check lock ordering so a worker waiting on the control mutex cannot
deadlock a remover waiting on that worker. Preserve sole data-loop ownership of
normal buffer recycling. Share only the small lifetime mechanism whose semantics
are actually common; a wholesale adapter rewrite is unnecessary.

**Required validation:** hold a real parameter callback after it starts, unlink
or renegotiate that parameter port, then release it and verify safe completion
before unmapping. Repeat with an unclaimed queued callback, `-EBUSY` retry,
completed-but-unrecycled parameter, retained conditional output, and full
destruction. Exercise both adapters under ASan/UBSan with a failing-before and
passing-after invariant. The existing main-event teardown test establishes a
different lifetime and does not close this finding. Recommendation: accept for
remediation; confirm exact live trigger paths before finalizing the fix.

### AO-REV-002 — Graph admission accepts malformed configuration prefixes

Affected code: `spa/plugins/filter-graph/filter-graph-ndarray.c:1284`, `:1329`,
`:1368`, `:1376`.

**Observed:** `parse_workers()` returns success once it has seen `helpers`,
without checking whether iteration ended because of a parse error. The graph's
node/link loops likewise stop when `spa_json_enter_object()` returns nonpositive,
without distinguishing a valid end from a nonobject element or malformed input.
No final full-input-consumption check is made.

**Observed experiment:** [graph-admission.c](review/graph-admission.c) calls
the real public graph constructor with the built C example plugin. Each of the
following is accepted with result 0 and a one-node graph:

```text
nodes = [ { valid scale-f32 node } 7 ]
nodes = [ { valid scale-f32 node } ] links = [ 7 ]
nodes = [ { valid scale-f32 node } ] workers = { helpers = 0 broken }
{ nodes = [ { valid scale-f32 node } ] } garbage
```

The ordinary valid configuration is also accepted, providing a positive control.
The listed node shorthand stands for the fully specified node generated by the
probe, not literal input accepted by the parser.

**Derived:** a damaged or mistyped graph declaration can be accepted with nodes
or links ignored, or with an incomplete worker declaration silently accepted.
This weakens admission precisely where the host otherwise validates formats,
schemas, port ownership, and unknown worker fields. Upstream audio filter-graph
code uses similar permissive container loops; this is a concrete FGN admission
issue, not evidence of inconsistent authorship or arbitrary style.

**Smallest coherent remediation:** explicitly validate every array member's
kind, propagate parser errors, require complete containers and the intended
top-level value, and keep PipeWire's documented relaxed JSON syntax. Do not
replace SPA configuration syntax with a strict-JSON-only parser.

**Required validation:** the four negative cases above, nonobject tokens before
and between valid entries, malformed/truncated worker fields, malformed links,
and trailing valid/invalid values must fail with no graph published. Preserve
comments, unquoted keys/strings, and all accepted examples. Recommendation:
accept for remediation; the existing passing suite does not test these cases.

### AO-REV-003 — The live FIFO test does not prove backlog-driven progress

Affected code: `src/tests/meson.build:106`,
`src/tests/test-ndarray-filter-remote.py:159`, `:174`,
`src/tests/test-ndarray-filter-admission.c:57`, `:213`;
production path `src/pipewire/ndarray-filter.c:732`, `:936`.

**Observed:** the live `pw-test-ndarray-filter-fifo` test runs the existing
retained-output scenario with FIFO enabled. It sends trigger 1, waits until the
first callback prints `DEFERRED`, and only then sends trigger 2. There is never a
demonstrated queued second input at the first callback's end. That test therefore
passes without proving the new backlog scheduling path. The focused admission
test does create two queued inputs and checks the event, but its
`pw_filter_trigger_process()` substitute only increments a counter and the test
calls the next processing cycle itself.

**Observed:** `pw_filter_trigger_process()` emits a `RequestProcess` request for
this non-driving filter. PipeWire does not guarantee that every such request
starts a graph cycle, the ndarray filter does not advertise
`node.supports-request`, and this policy has no acknowledgement, timeout, or
retry if the driver ignores or coalesces the request. Output starvation also
retains an input and returns without requesting another cycle.

**Derived:** local per-port order and event scheduling are tested, and connected
conditional-output retention is tested, but the integration from retained
backlog through RequestProcess to driver progress remains unproved. The helper
cannot guarantee forward progress or end-to-end losslessness. Its useful
contract is narrower: retain the oldest helper-visible buffer in a bounded
PipeWire pool until a usable cycle consumes it or processing terminates.

**Smallest coherent remediation and validation:** create a live source with a
bounded burst already queued, a driver that services RequestProcess, and no
external trigger after the burst. Require every expected sequence exactly once
and in order. Withhold then return output capacity and require recovery without
a new input arrival. Add two-input skew, FIFO plus independent-input absence
tokens, and restart/pool-removal cases. As a negative control, disabling the
backlog request must fail the progress assertion. Recommendation: retain the
bounded mechanism, rename or qualify its public contract, and add this coverage
before relying on it for loss-intolerant acquisition.

### AO-REV-004 — Mechanism benchmarks do not establish deployment latency bounds

Affected contract: `doc/dox/internals/filter-graph-ndarray.md:49`, `:309`, `:321`;
implementation `spa/plugins/filter-graph/ndarray-executor.c:99`, `:181`, `:553`;
outer scheduling `src/pipewire/data-loop.c:80`,
`spa/plugins/support/loop.c:759`.

**Observed:** the FGN contract explicitly has no admitted percentile target.
Workers busy-poll and the coordinator waits until all helpers finish; creation
uses ordinary `pthread_create()` without per-helper affinity. The polling loop
scans all configured sources and executes up to 32 control invocations per
handoff. A callback's duration remains a deployment responsibility; the
contention handoff can sleep and reacquire a mutex. FIFO backlog requests and
property/parameter notifications pass through event sources and the main loop.

**Observed:** the maintained benchmark documents distinguish closed-loop
service/wake latency from camera arrivals, queuing, device I/O, and final command
latency. The traceability ledger explicitly records missing end-to-end overload,
runtime interposition, target-core, and AArch64 numerical qualification. This
honest scope statement should be preserved, not replaced by a blanket claim
that lock-free or allocation-free mechanisms bound wall-clock time.

**Derived:** a finite helper count does not bound rendezvous wall time if a
helper is preempted or starved. A bounded FIFO controls storage, but can carry
stale work and cannot guarantee camera losslessness once acquisition outpaces
service. At arrival rate lambda and mean residence time W, resident work is
L = lambda W; extra buffer capacity permits extra age. A controller's accepted
age bound and overflow/abandonment policy must therefore accompany its pool size.

**Required acceptance work:** choose the input-ready and final-command event
boundaries, realistic independent arrival and burst processes, rows/frame and
matrix sizes, allowable loss/age, throughput and tail/deadline targets, CPU/SMT/
NUMA/IRQ placement, worker and control-thread scheduling, and cold/restart
requirements. Measure open-loop end-to-end distributions, deadline misses,
maximum backlog age, drops, recovery time, CPU use, faults, and per-scan control
interference. Qualify actual camera and actuator interfaces separately.
Recommendation: keep this an explicit deployment gate. No arbitrary gains,
thread-priority, affinity, or buffer-size changes are justified by this review.

### AO-REV-005 — The ledger has useful coverage but weak replay identities

Affected artifact: `doc/dox/internals/filter-graph-ndarray-traceability.toml:1`
and its `verified_revision`, `verified_environment`, and external link fields.

**Observed validation:** Python `tomllib` accepts the file. All 19 requirement
IDs are unique and exactly match the 19 normative headings. All repository-local
implementation/evidence paths exist. Thirteen entries are partial and six are
validated. There are 51 distinct sibling-repository references. A repository
search finds the normative document's pointer to this ledger but no maintained
validator or executable consumer.

**Observed:** several verification identities are prose such as “working trees
reviewed 2026-08-25” or a commit “plus the test change.” Those do not identify the
complete replayable source. Sibling-relative paths cannot independently identify
an external checkout and commit. Broad entries also mix the C host with external
Rust/Julia/generated-plugin claims and artifact campaigns.

**Derived:** the file is useful as a coverage and open-gap map, but a reviewer
cannot recover an exact cross-repository validated configuration from some
entries. The risk is evidence drift and repeated expensive investigation, not
runtime overhead or a need for a larger compliance system.

**Smallest coherent remediation:** retain a compact local ledger; replace prose
baselines with immutable commits/build identifiers where available, attach a
command/result/artifact digest to promoted validation, and identify external
repositories explicitly. Keep detailed Calculon/Julia/product campaign tracking
with its owner, linking to a pinned summary from this repository. A small
Python validator for IDs, required fields, local paths, and closure completeness
would detect cheap errors; it should not infer that an existing test path means
a requirement passed. Recommendation: accept as maintenance work, without
promoting currently partial claims.

## FIFO decision

**Keep the opt-in retain-oldest mechanism, but do not freeze the current
`FIFO_INPUTS`/lossless public contract unchanged.** A progressive row algorithm
needs every contributing block of an admitted frame; draining two ready blocks
to the newest one can destroy the frame's meaning. The implementation retains
one dequeued input per port and relies on the finite negotiated PipeWire pools
rather than allocating a second application queue. Rename or document it as
ordered helper admission or retained-oldest input, not an end-to-end FIFO.

Keep its claim narrow: no helper-side drop of a valid dequeued frame input
before delivery or termination. FIFO does not guarantee source-device
losslessness, physical-time freshness, aligned multi-camera acquisition, or
progress from a driver that does not service requests. In independent-input
mode the callback may receive one input without its peers; it still owns the
semantic acquisition-key join. Zero-length absence tokens are intentionally
excluded from frame admission.

AO-REV-003 must close the live progress gap before the mode is relied on for
loss-intolerant acquisition. AO-REV-001 must close retention lifetime failures.
Operational pool capacities need a latency/age budget and a deliberate response
when a camera cannot be backpressured. For an observer,
latest-value behavior remains a useful default. For the synchronous FGN module,
one input is ordinarily delivered per graph cycle; do not automatically copy
FIFO machinery into that adapter without demonstrating a supported topology
that accumulates frame arrivals there. The current helper flag and the module
are different public surfaces and should be documented as such.

## Architecture, ownership, and scope

| Stage/state | Writer/owner | Capacity and publication | Important limit |
| --- | --- | --- | --- |
| Device/source pool and one published quantum | SPA source process owner | Negotiated pool; SPA_IO_Buffers; graph dependencies | Device overflow policy is external; polling driver waits for cycle completion |
| Activation status/timestamp | Final dependency producer, then destination process owner | Atomic activation transition and v2 timestamp publication; polling or eventfd per target | Same graph ordering; wake policy is not a new scheduler |
| Standalone retained frame inputs/outputs | Filter process owner | One retained pointer per port plus ordinary finite queues | FIFO join/progress and pool removal need the above checks |
| Parameter handoff | Data owner publishes; serial worker borrows; data owner recycles | One pending buffer per parameter port; release/acquire flags | New updates can be rejected; removal must quiesce worker |
| Graph property/parameter transactions | Serial control preparer; graph-cycle commit owner | Bounded pending/retired state | Preparation may allocate/block; failed processing has no graph-wide rollback |
| Internal FGN buffers and node state | One graph coordinator | Preallocated node outputs; topological synchronous calls | Repeated validation scales with admitted graph and port cardinality |
| Fixed executor lanes | Coordinator publishes; each helper writes completion | One command per helper; release/acquire generation and completion | Synchronous wait requires each helper to remain runnable |
| Main-loop publication/error/request events | Data/worker producer; main-loop consumer | Coalesced preallocated events | Kernel/control scheduling is outside the inner graph's syscall-free scope |

The generation-wrap rearming and cache-line separation in the executor encode
real publication and false-sharing concerns. Do not simplify them as boilerplate.
The scalar/AVX2/NEON reduction profiles also have a numerical reason: helper
counts do not change one task's result, and the specified profile matches the
same-target Calculon calculation. Moving kernel policy in a later ABI is a
possible architectural improvement, not a correctness fix justified by this
review. Splitting work across ACCUMULATE calls changes floating-point grouping;
the documentation already acknowledges that separate equivalence obligation.

No busy-spin default is imposed. Ordinary unqualified nodes are kept off polling
loops, source probing starts after Start, and the former camera plugins live
outside the core. These are appropriate compatibility and lifecycle decisions.
Retained plugin libraries can consume process-lifetime resources; the contract
requires bounded per-library registry state, but a product that loads unlimited
distinct library paths still needs an admission/restart policy.

The graph's finite cardinality limits do not by themselves establish a useful
CPU bound. Outer input/output overlap validation scales with the number of
ports and metadata regions, and the graph also checks outputs against other
buffers and graph-owned arrays. Preserve trust-boundary validation unless a
prepared proof can replace repeated work; measure representative graph sizes
before optimizing these checks. Optional helper lanes add continuously runnable
threads rather than automatically reducing latency.

## ABI, build, and maintenance decisions

The public plugin boundary uses versioned C layouts, fixed-width fields,
`struct_size` guards, explicit pointer lifetimes, and append-only optional
descriptor callbacks. Ndarray and Acquisition IDs are explicitly reserved.
Polling activation v2 uses an existing padding word and checks version support;
v0/v1 eventfd compatibility is preserved in selected trigger paths. The local
tests cover the changed ABI shape and mixed scheduling mechanisms, but arbitrary
old binary peers and non-x86 targets were not independently qualified here.

The fork has distinct library/pkg-config, header, module, SPA plugin,
configuration/XDG, executable, socket, and service names. Keeping `pw_` symbols
requires separate processes from system PipeWire, as the README now states.
That is a documented coexistence boundary, not a claim of safe mixed-library
loading. Default AO build options and focused tests pass. Optional desktop
compatibility builds, installed manpage collisions, and a full packaged install
matrix remain outside the executed checks. Retained upstream README audio/tool
instructions deserve small editorial cleanup; they are not architecture defects.

Two bounded support-surface decisions are worth making before freezing more ABI:

- `filter-graph-ndarray.h` and `ndarray-executor.h` declare direct host functions,
  and all SPA headers are installed, but `spa/plugins/filter-graph/meson.build:1`
  builds their implementation as an uninstalled static library. The module uses
  it internally. Decide whether direct external host users are supported by an
  installed link target/pkg-config contract or whether those declarations are
  explicitly internal. Plugin descriptor/executor interfaces can remain public
  without promising an independently packaged host library. This is an observed
  packaging ambiguity, not proof that the current module workflow is broken.
- The unused `spa_ringbuffer_shared` public surface was retired after the
  review. Its former transport consumer had already been removed, and no caller
  was found in PipeWireAO or the inspected sibling repositories. The upstream
  `spa_ringbuffer` ABI remains unchanged. `SPA_CACHE_LINE_SIZE` remains because
  the ndarray executor uses it for its private worker layout.

The two adapters have related but distinct responsibilities: a scientific owner
callback/owned main loop versus an in-process composite plugin graph. Their
coexistence is justified. Centralize only rules that already must agree, such as
the retained-buffer removal contract and small format-validation primitives.
Large generic managers or a second scheduler would increase the maintenance
burden without resolving the demonstrated defects.

## Traceability file decision

**Keep `doc/dox/internals/filter-graph-ndarray-traceability.toml` for now.** It is
the only maintained per-requirement map of implementation, verified surfaces,
and explicit gaps, and the normative document points to it. Its 19 IDs agree
with the specification. It does not affect runtime and does not presently claim
the whole capability is complete. Deleting it without moving those obligations
elsewhere would lose useful engineering context.

Apply AO-REV-005: narrow it to PipeWireAO authority, pin external evidence, and
remove repetitive campaign detail once a durable owner-repository record
replaces it. Do not create an elaborate new traceability framework. This review
did not modify the ledger or its normative requirements.

## Verification record and next acceptance steps

Primary-agent verification at the reviewed tip on 2026-09-10:

- `meson compile -C build`: passed, including regeneration and 606 build targets.
- All 16 focused tests passed: `spa-filter-graph-ndarray-c`,
  `spa-filter-graph-ndarray-retained-c`, `spa-filter-graph-ndarray-rust`,
  `pw-test-ndarray-filter`, `pw-test-ndarray-filter-admission`,
  `pw-test-ndarray-filter-publication`, `pw-test-ndarray-filter-chain-parameter`,
  `pw-test-run-control`, `pw-test-polling-data-loop`, `pw-test-polling-remote`,
  `pw-test-ndarray-filter-remote`, `pw-test-ndarray-filter-fifo`,
  `pw-test-ndarray-filter-parameter`, `pw-test-ndarray-filter-props`,
  `pw-test-ndarray-filter-chain-props`, and `pw-test-ndarray-filter-chain-teardown`.
- `git diff --check`: passed.
- Full configured Meson suite: 56/56 passed, with no failures, skips, or
  timeouts.

Independent reviewer checks: skill hotspot scan; TOML parse/ID/local-path
validation; the two executed negative probes recorded above. The primary build
at `/home/dgamroth/workspaces/codex/pipewire/pipewire/build` supplied matching
generated headers, libpipewire, and the FGN static library/example plugin to the
probes. No hardware, open-loop controller campaign, or broad sanitization
campaign was rerun in this review.

Reproduce the investigative probes from a checkout with a matching normal build
(replace `build` with that build directory):

```sh
cc -std=gnu11 -D_GNU_SOURCE -DFASTPATH -fno-strict-aliasing -fno-strict-overflow \
  -Isrc -Ispa/include -Iinclude -Ibuild -Ibuild/src -Ibuild/spa/include \
  docs/review/parameter-removal.c -Lbuild/src/pipewire \
  -lpipewire-ao-0.3 -pthread -o /tmp/ao-review-parameter-removal
LD_LIBRARY_PATH="$PWD/build/src/pipewire" \
  PIPEWIREAO_SPA_PLUGIN_DIR="$PWD/build/spa/plugins" \
  /tmp/ao-review-parameter-removal

cc -std=gnu11 -D_GNU_SOURCE -Ispa/include docs/review/graph-admission.c \
  build/spa/plugins/filter-graph/libspa-filter-graph-ndarray.a \
  -ldl -lm -pthread -o /tmp/ao-review-graph-admission
/tmp/ao-review-graph-admission \
  "$PWD/build/spa/plugins/filter-graph/libspa-filter-graph-ndarray-example.so"
```

The first probe currently expects a child SIGSEGV to establish the defect; its
successful exit is not a passing safety regression. Convert it into a
no-stale-borrow/no-fault regression when repairing AO-REV-001. The second prints
the accepted invalid configurations; turn those into negative assertions for
AO-REV-002. Preserve a valid positive control in both cases.

Recommended sequence: adjudicate the demonstrated lifetime/admission findings;
establish the live failing scenarios; apply narrowly scoped fixes; verify both
adapters and control/data teardown independently; add the FIFO progress test;
then qualify the chosen deployment's timing and hardware surfaces. No production
changes were made during this review.

### AO-REV-003 connected qualification evidence (2026-10-06)

The new `pw-test-ndarray-filter-fifo-queued` fixture establishes eight original
inputs already queued behind a controlled producer output-capacity hold. Its
stock helper trace proves exact sequence/row-offset order and same-activation
prefetch. Returning capacity drains the original inputs without publishing
another input; the conditional first output is deliberately unavailable and
all seven expected artifacts are received. The native negative control disables
only the helper backlog request and stalls at two callbacks with helper error
zero. See [NATIVE_FIFO_VALIDATION.md](NATIVE_FIFO_VALIDATION.md) for the exact
public API fault-injection seam, original logs, sanitizer scope, and remaining
gates. Primary review is required before promoting the issue's disposition;
this evidence does not qualify scientific pacing, overload, or all drivers.
