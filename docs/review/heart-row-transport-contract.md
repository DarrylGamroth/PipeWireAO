# HEART row block transport: contract and implementation boundary

Source revisions: PipeWire `e26c9f22d`; HEART SPA plugin `9dff9df`.
Worktrees: `pipewire-row-transport-contract` and `heart-row-transport-contract`,
both on `codex/row-transport-contract` branches. No production transport change
has been made.

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
   The FGN chain currently dequeues all input buffers, keeps the newest, and
   returns the older ones before processing. It also returns its current input
   when any output is absent. Both behaviors violate exact FIFO delivery for
   row blocks.

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
