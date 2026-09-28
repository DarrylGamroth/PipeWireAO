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
its own active RT mix list. That substitution alone has no lifetime proof:
`pw_impl_link_destroy()` clears both `mix->peer` pointers before it calls
`input_remove()` and `output_remove()`. A driver callback may still be using
an active output mix at that point. Format changes remove a target input port
from its data loop before mutating its mixer and buffers, without acquiring
the source data loop lock. Whether graph deactivation always precedes that
mutation is unproven. A cross-loop peer or shared IO pointer needs an
explicit quiescence rule before either mutation.

Next experiment: make a two-loop test pause the driver after it selects an
active output mix and before it reads the peer. On the control thread,
deactivate and destroy that link, and separately reconfigure the target input
port while the source output port remains active. Record whether each path
blocks until the driver callback exits. Add a test for input-port addition to
an active target with another port. Only then replace the driver scans with a
driver-loop-owned view and move peer clearing after its synchronous removal.
The consumer publisher needs the analogous barrier test for mix removal.
The lifetime proof must also cover ordinary non-row processing on the same
RT lists.

The quiet-host Rust replay r3 reported 2048 captured WFS packets, 1023 HEART
frames published, one frame dropped, and one buffer starvation; later frames
continued. Its files are at
`/home/dgamroth/.cache/copper-phase2-quiet-rust-progressive-474-r3-20260928/`.
This is a transient starvation observation, not evidence of RRF-001's
persistent circular wait. Concurrent build and replay activity prevents a
controlled latency comparison with other runs.
