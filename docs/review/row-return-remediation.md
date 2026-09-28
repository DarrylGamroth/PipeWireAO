# Borrowed row return remediation

Base: `04dd90cb82c932fe8f156c8336616726ef6332d7` on
`codex/row-transport-contract`. This worktree is
`pipewire-row-return-remediation` on `codex/row-return-remediation-20260928`.
The independent review is `row-return-final-review.md` in the review worktree.

## RRF-001: retained row retry

The new `test_retained_row_retry()` in `test-row-transport-order.c` failed on
the base source at `retry_commands == before + 1`: the completed graph had a
borrowed row, and `flush_reliable_retry()` suppressed the command. It passes
with the change. The source command is now dispatched once after all prior
activations finish and pending exact releases have been scanned. The callback
may start a cycle with the loan still held only when no source output port has
`HAVE_DATA`. The source tee therefore has no new row to publish in that cycle.
The loan and its ID remain unchanged until the consumer returns the exact ID
after the retry graph completes. A second command waits for the first
command's callback. Unprepare clears the dispatch state.

The focused test checks the command, rejects a ready callback that carries a
source row, admits the no-data callback, and rejects another cycle while the
retry target remains awake. Existing mix and output-return tests check exact
ID reuse and the filter's `RequestProcess` event. These are separate focused
tests: no complete live exported-client replay of the retained-output
transition has been run on this branch.

The supported HEART row source declares `max_output_ports = 1`. The core
property `pipewireao.row-transport` does not itself enforce that limit. The
current source-row guard checks all active output ports, so an unrelated
output with `HAVE_DATA` on a generic multi-output source can suppress a
retained-row retry. Multi-output row sources are outside the qualified
topology; they need an explicit row-output marker or setup rejection before
the core contract can be generalized.

Verification: `meson test -C build-row-remediation
pw-test-row-transport-order pw-test-row-transport-mix
pw-test-filter-output-return --print-errorlogs` passed 3/3 with GCC 14.2.0,
`debugoptimized` and `-O2`. The baseline failure was observed before changing
production source. No latency measurement or sanitizer run is claimed.

## RRF-002: list membership and lifetime

The driver still scans `target->node->input_ports` and each port's
`mix_list` from its data loop. The target port list is mutated by the control
thread in `pw_impl_port_add()` and `pw_impl_port_remove()`. The consumer's
`pw_impl_port_publish_row_return()` also scans the control-thread `mix_list`
from its data loop; `pw_impl_port_init_mix()` and
`pw_impl_port_release_mix()` mutate it without that loop's exclusion. These
are unsynchronized list and object-lifetime accesses. RRF-002 remains open.

The existing `rt.input_mix`, `rt.output_mix`, and private `rt.mix_list` are
updated through `pw_loop_locked()`. A safe driver scan could start from its
own output ports and active output mixes, and the consumer publisher could use
its own active RT mix list. That substitution needs a lifetime protocol.
`pw_impl_link_destroy()` calls `pw_impl_link_deactivate()` before clearing
`mix->peer`; deactivation removes the default tee's output mix synchronously
under the source loop lock. This path gives the source reader a quiescence
point before peer clearing. The earlier note in this branch incorrectly put
peer clearing first.

Other mutations bypass that ordering. Non-NULL `impl-port.c:port_set_io()`
replacement assigns `mix->io[]` before `pw_loop_locked(do_add_mix)`; when the
mix is already active, the locked callback does not remove it first.
`pw_impl_port_set_param(Format)`, `pw_impl_port_use_buffers()`, and
`pw_impl_port_set_mix()` can change a target's mixer or buffers without first
removing the corresponding active source output mix. The driver would retain
a cross-loop peer pointer during those changes. The source-side mix must be
removed and its reader quiesced before target mutation, followed by consumer
loop quiescence. Neither loop lock should be held while waiting for the other.

The exported client adds a protocol boundary. Server
`client-node.c:impl_mix_port_set_io()` updates the server mix pointer and
`do_port_set_io()` enqueues a native `port_set_io` message. Client
`remote-node.c:client_node_port_set_io()` applies it later. Server-side loop
quiescence does not establish that the client stopped using the old shared IO
or buffer generation. `spa_node_sync()` can produce a native ping/pong, but
using that result to gate generation reuse requires an asynchronous detach
transaction and an explicit failure path if the client disappears. A held
loan must be returned by exact ID or revoked only after both loops and the
client have quiesced. This transaction is outside the narrow list-scan edit;
no partial RT-list swap was made.

Deterministic validation plan:

1. On two real data loops, pause the driver after selecting an active source
   output mix. Start link destruction from the control thread. Assert that
   peer clearing and free wait for the source scan to resume.
2. Repeat with Format, buffer renegotiation, and mixer replacement while the
   source output mix is active. Require source-side removal before mutation,
   consumer-loop quiescence before reuse, and no old-generation return.
3. Pause the consumer publisher after selecting an active mix. Start mix
   removal, then require it to wait before clearing IO or freeing the mix.
   Add an unrelated input port to a target that remains active through a
   different port; ordinary processing must remain valid.
4. For exported clients, delay the detach message and then the ping/pong
   acknowledgment. Require no buffer-generation reuse before acknowledgment;
   disconnect and timeout must leave the loan revoked or quarantined without
   touching freed memory. Run a sanitizer build and a bounded stress replay
   after the barrier tests pass.

The quiet-host Rust replay r3 reported 2048 captured WFS packets, 1023 HEART
frames published, one frame dropped, and one buffer starvation; later frames
continued. Its files are at
`/home/dgamroth/.cache/copper-phase2-quiet-rust-progressive-474-r3-20260928/`.
This is a transient starvation observation, not evidence of RRF-001's
persistent circular wait. Concurrent build and replay activity prevents a
controlled latency comparison with other runs.
