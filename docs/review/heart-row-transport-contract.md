# HEART row block transport: contract and implementation boundary

Initial source revisions: PipeWire `e26c9f22d`; HEART SPA plugin `9dff9df`.
Worktrees: `pipewire-row-transport-contract` and `heart-row-transport-contract`,
both on `codex/row-transport-contract` branches. The initial investigation
and fail-before results below are retained as the baseline for the repair.

## Reproducible failure

`HEART_ROW_TRANSPORT_CONTRACT=1` runs the opt-in assertion in
`spa/plugins/heart/test-source.c` in the HEART worktree. It sends two valid
row packets to an ephemeral loopback port. After the first packet, the test
copies its descriptor into a pending mix IO and changes the source IO to
`NEED_DATA`, matching the default tee. It withholds consumer release to model
an input mix waiting while the consumer has no output buffer. The contract
requires one source `ready` call until the pending block is consumed and
acknowledged. Current code makes two; the test fails at
`capture.ready_calls == 1`. The ordinary source test passes without this
opt-in assertion. This unit test models the tee transfer; the measured
tee/mix and activation traces in the JuliaFilterGraph camera overlap report
establish the same replacement in the live topology.

The actual core tee/mix regression in `src/tests/test-row-transport-mix.c`
also fails before production changes. Its reliable tee retains block ID 0
while the consumer cannot release it, and its input mix transfers that ID.
The test then invokes the consumer's release callback and expects one release
of ID 0 at the source. `schedule_mix_reuse_buffer()` returns success without
forwarding it, so the `source.releases == 1` assertion fails. These commands
and raw outputs are captured in `docs/review/heart-row-transport-evidence/`:

```text
cd /home/dgamroth/workspaces/codex/pipewire/pipewire-row-transport-contract
ninja -C build-row-contract src/tests/pw-test-row-transport-mix
build-row-contract/src/tests/pw-test-row-transport-mix

cd /home/dgamroth/workspaces/codex/pipewire/heart-row-transport-contract
ninja -C build-row-contract spa/plugins/heart/spa-heart-source-test spa/plugins/heart/libspa-heart.so
build-row-contract/spa/plugins/heart/spa-heart-source-test build-row-contract/spa/plugins/heart/libspa-heart.so
HEART_ROW_TRANSPORT_CONTRACT=1 build-row-contract/spa/plugins/heart/spa-heart-source-test build-row-contract/spa/plugins/heart/libspa-heart.so
```

Core and opt-in HEART cases exited with SIGABRT (`-6` in the captured Python
subprocess results); the ordinary HEART case exited 0. The core test uses no
socket. The HEART tests reserve an OS-assigned loopback UDP port.

## Existing ownership and scheduling

1. `tee_process()` in `src/pipewire/impl-port.c` copies the source IO to each
   mix and immediately sets source IO to `NEED_DATA`. That transition is a
   descriptor transfer, not consumer release. `recycle_buffer()` in the HEART
   source treats this same status as release when the IO still carries an ID,
   so it can also make a still-pending payload writable. A second publication
   can replace a pending mix descriptor.
2. `tee_process_reliable()` leaves a pending descriptor in place, but its
   source IO return only says the mix slot was available. It does not say the
   filter finished using the buffer. `schedule_mix_reuse_buffer()` has a FIXME
   instead of peer forwarding. The HEART source's `port_reuse_buffer()` returns
   `-ENOTSUP`.
3. `node_ready()` in `src/pipewire/impl-node.c` resets downstream activation
   on a new source cycle even when the prior activation was unfinished. The
   source has no completion signal with which to gate its next `ready` call.
4. The Julia ndarray filter retains a FIFO input while any output is absent,
   then returns early. It schedules backlog after a successful process, with
   no proved wakeup when an output becomes available during a retained input.
   The FGN chain at the initial revision dequeued all input buffers, kept the
   newest, and returned the older ones before processing. It also returned its
   current input when any output was absent. Both behaviors violated FIFO
   delivery for row blocks. The separate opt-in FGN FIFO patch is integrated
   after the core transport commit.

## Required contract before a production patch

* Admission: the source reserves one slot for every row block of a frame when
  its first valid packet arrives. Its capacity is the number of whole frames
  that fit in negotiated, preallocated SPA buffers. If reservation fails, it
  rejects that entire frame, increments a **whole-frame overflow** counter
  once, and marks the next admitted frame discontinuous. Camera receives
  continue without waiting for the graph. A malformed frame also has an
  explicit once-per-frame count, separate from capacity overflow.
* Ownership: each reserved block keeps its buffer ID, frame sequence, row
  offset, flags, and pixel bytes unchanged from publication until a consumer
  release for that exact ID. An IO `NEED_DATA` transition at the tee is never
  treated as release. Invalid or duplicate release is a protocol error.
* Scheduling: at most one published block is outstanding. The next source
  `ready` call occurs only after the prior downstream activation has finished
  and the block has been released. A release callback generated inside the
  filter process must be deferred until that activation is finished; a
  cross-thread queued callback alone does not establish this order.
* Consumer: both Julia and FGN retain the oldest queued row input when an
  output is unavailable. They release an input only after graph processing
  accepts it. Output return must cause a process retry even if the camera
  publishes no further row blocks. The retry must be event driven or bounded
  without an unbounded hot loop.
* Topology: select this contract only for a single source-to-filter consumer
  link, or define a release reference count for fanout. Require the reliable
  tee behavior to keep a pending descriptor immutable. Reject incompatible
  topology at setup, rather than silently falling back to latest-buffer
  behavior.

The SPA node API has `reuse_buffer` and `port_reuse_buffer`, but the generic
mix does not currently carry consumer release to the source. More critically,
the API does not give this source a downstream activation-complete event.
Implementing an exact release/activation acknowledgment across the graph and
an output-availability retry is a core scheduler contract, not a local HEART
buffer change. A source-only queue, `node.reliable=true`, or Julia FIFO alone
would still permit loss or a retained-input deadlock.

## Patch sequence and required evidence

1. Add a core test with the actual tee, mix, and filter path. Publish two row
   blocks before the first input mix; withhold all filter outputs; then return
   one output. Assert exactly one mix handoff and no second source cycle before
   release, followed by both blocks in order without an additional camera
   packet. Include a delayed activation completion to expose early callbacks.
2. Define a core release path and completion ordering, including per-link
   buffer ID mapping, fanout policy, teardown behavior, and an output-return
   process trigger. Test a returned ID through each hop and prove no duplicate
   release through the legacy IO return path.
3. Implement source reservation, fixed-capacity FIFO publication, immutable
   block storage, exact ID release, once-per-frame capacity overflow, and
   discontinuity. Keep the full-frame mode on its existing path. Extend the
   opt-in source regression to drain the queued second block after the first
   release, and to check payload/header identity and capacity overflow.
4. Make FGN consume oldest-first and hold its input across output starvation.
   Verify Julia FIFO's output-return retry and correct it where needed.
5. Run the same fail-before cases as pass-after checks for both consumers.
   Then perform finite exact-delivery replays with controlled row spacing and
   output starvation. Record block IDs and frame IDs at source publication,
   tee, mix, filter arrival, filter release, and consumer output. Require
   no missing or duplicate block within reserved capacity; test overflow
   separately and check the count is one per rejected frame. The full-frame
   source test must continue to pass.

No throughput or latency claim follows from the unit regression. Hardware or
bench validation remains separate from software delivery verification.

## Implemented opt-in handoff

`pipewireao.row-transport=true` selects this contract on the HEART row source.
The source also sets `node.reliable=true` to select PipeWire's reliable tee.
Existing reliable sources without the row property retain their old behavior.
Link setup rejects row-source fanout and incompatible fan-in. The input mix
passes a returned buffer ID through its reciprocal tee peer only for the row
source. The returned ID is taken from consumer-owned input IO after that
consumer and every active driver target have reached `FINISHED`; the tee's
`NEED_DATA` descriptor transfer is never treated as a release. The source
rejects an invalid or duplicate ID and publishes the next row only after the
exact ID is returned.

The driver data loop scans the active target snapshot after graph completion.
A pending release bit remains set through the entire scan, so a filter output
return on another loop cannot request a new source cycle during release. The
filter's opt-in output return hook raises `RequestProcess`; it sets a pending
bit and signals a preallocated driver-loop event. A late output return thus
wakes the driver without a new camera packet. The driver sends the command to
HEART only after the release scan and graph completion. A failed command while
running remains pending and emits an incomplete event. The source and impl
node share the driver data loop through `node.loop.name`; HEART's release and
request handlers enqueue eventfd work, so `ready` cannot synchronously reset
the graph inside the release callback.

HEART reserves every buffer for a frame when its first valid row arrives.
The negotiated buffer count must hold at least one frame. It writes each row
once into its reserved slot, queues its immutable ID and payload, and keeps at
most one published ID in flight. A capacity failure drops and counts the
whole frame once while UDP reception continues. No row path allocates per
block. Full-frame mode keeps its existing publication path.

Pause keeps a published ID borrowed until exact release. Link deactivation
may detach IO with `port_set_io(NULL)` without recycling that ID; a non-NULL
replacement is rejected while it is in flight. After graph quiescence,
`use_buffers(0)` revokes the SPA buffer lifetime and clears outstanding row
state. The caller must not deliver old-generation release callbacks after
that revocation. The SPA callback carries only a buffer ID, so it cannot
distinguish an old callback from a new use of the same ID after rebinding.

## Focused verification

The raw fail-before and pass-after outputs are in
`docs/review/heart-row-transport-evidence/`. The three core cases use the
actual tee/mix, filter output return, and impl-node scheduler code. The
two-loop order case blocks the driver inside its release scan while a filter
thread requests retry, then checks command ordering and a late return event
without a camera packet. The HEART case uses an OS-assigned loopback port; it
checks two fast rows, retained output, exact and duplicate release, whole-frame
overflow, malformed-frame recovery, Pause, IO detach, and buffer revocation.

```text
cd /home/dgamroth/workspaces/codex/pipewire/pipewire-row-transport-contract
meson test -C build-row-contract pw-test-ndarray-filter-fifo pw-test-row-transport-mix pw-test-row-transport-order pw-test-filter-output-return --print-errorlogs

cd /home/dgamroth/workspaces/codex/pipewire/heart-row-transport-contract
ninja -C build-row-contract spa/plugins/heart/libspa-heart.so spa/plugins/heart/spa-heart-source-test
build-row-contract/spa/plugins/heart/spa-heart-source-test build-row-contract/spa/plugins/heart/libspa-heart.so
HEART_ROW_TRANSPORT_CONTRACT=1 build-row-contract/spa/plugins/heart/spa-heart-source-test build-row-contract/spa/plugins/heart/libspa-heart.so
```

The four core tests and both HEART modes pass at this revision. This is
focused software verification. An installed-prefix JFG/FGN finite replay and
matched live qualification remain separate integration checks.
