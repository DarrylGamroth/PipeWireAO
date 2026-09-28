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

The baseline failure was observed before changing production source. The six
focused tests listed below pass with GCC 14.2.0 in the `debugoptimized`
build. No sanitizer result is claimed.

## RRF-002: list membership and lifetime

The consumer publisher now reads its own private active RT mix list. Its
`port_set_io()` updates IO pointers and RT membership on the owning data loop,
and `pw_impl_port_release_mix()` waits for that loop before removing the mix
from the control list. The driver scans its own active output ports and each
output port's private RT mix list. It no longer traverses a target node's
control-thread `input_ports` list. Normal link destruction deactivates the
source output mix and waits for its data loop before clearing reciprocal peer
pointers.

Ordinary local input mixes do not set `row_transport`; the driver reads their
consumer IO after graph completion. Exported-client server mixes set
`row_transport` at mix initialization; the driver reads returned IDs through
the shared link IO. The server node has `remote=true`, whereas the actual
client node has `exported=true`. The focused fixture represents them as
separate nodes. The driver uses its own output mix's IO pointer, which link
activation sets to the same shared storage as the input mix. The independent
review found no further concrete defect in this fixed-graph steady-state
path.

Format changes, buffer replacement, and mixer replacement while a link is
active remain unqualified. The tests do not establish that an old exported
client has acknowledged detach before its shared IO or buffer generation is
reused. These cases need a defined quiescence and generation contract before
claiming general live reconfiguration safety.

Fail-before evidence for all three list/IO races is in
`row-return-lifetime-tests.md`. The corrected branch passes the six Meson
targets `pw-test-row-return-mix-removal`,
`pw-test-row-return-io-replacement`,
`pw-test-row-return-driver-lifetime`, `pw-test-row-transport-order`,
`pw-test-row-transport-mix`, and `pw-test-filter-output-return`.

A real exported Rust FGN Copper replay against this core delivered all
2,048 WFS datagrams, published all 2,048 row blocks, and emitted 1,024 DM
commands with zero source drops or buffer starvations at 474 Hz and 2,000 µs
readout. Its command trajectory differed from the complete-frame numerical
reference by at most 3.73 × 10⁻⁸ µm. The raw run is
`/home/dgamroth/.cache/copper-row-remediation-rust-1024f-20260928/`;
its WFS table and packet capture are gzip archived. Its terminal-packet-to-DM
p50/p99 was 197/348 µs, close to the prior unmodified-core distribution.
This finite replay qualifies exact delivery at that spacing, not dynamic
reconfiguration or a worst-case deadline.

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
