# RRF-002 lifetime probes

Source revision: `319b437d94e48b52873ad26c90cdedfb46eae035`.
Test branch: `test/row-return-lifetime-20260928`.
Both cases start separate PipeWire driver and consumer data loops. The consumer
loop executes the production row-return publisher. A test-only iterator hook
pauses it immediately after mix selection. The mix and both IO descriptors stay
allocated until the reader completes.

The removal case calls the production `pw_impl_port_release_mix()` on a control
thread while the consumer is paused. The test expects removal to wait for the
reader. On the source revision above it fails:

```text
'!removed_while_selected' failed at src/tests/test-row-return-lifetime.c:234 main()
Exit status: 134
```

The replacement case calls the production `port_set_io()` for an active mix.
The test observes the control thread reaching `pw_loop_locked()` and requires
the old IO pointer to remain installed until the consumer resumes. On the same
source revision it fails:

```text
'old_io_retained' failed at src/tests/test-row-return-lifetime.c:229 main()
Exit status: 134
```

The test was compiled at `-O0` against the matching
`pipewire-row-return-remediation/build-row-remediation` generated headers and
library. The Meson targets are `pw-test-row-return-mix-removal` and
`pw-test-row-return-io-replacement`.

## Driver scan of an unrelated target port

`pw-test-row-return-driver-lifetime` runs the production
`driver_has_borrowed_row()` on a PipeWire driver data loop. The target has a
row input and an unrelated input; a source output mix remains active for the
row input. The iterator hook pauses the driver immediately after it selects
the unrelated input. A control thread calls the production
`pw_impl_port_remove()` for that input. That routine excludes the target's
data loop, then removes the control-list entry. The port storage is retained
until the driver completes.

On the same baseline, at both `-O0` and `-O2`, removal completes while the
driver remains paused:

```text
'!removed_while_selected' failed at src/tests/test-row-return-driver-lifetime.c:159 main()
Exit status: 134
```

The test also accepts a driver implementation that scans only its own active
output mixes: in that case the unrelated target port is not selected by the
driver and removal is allowed after the scan completes. This probe covers
the borrowed-row scan; it does not run `flush_reliable_input_returns()` or
destroy a link.

The consumer probes use actual PipeWire data-loop threads and the production
publisher, release, and IO replacement methods. Their driver loop is running
but does not execute a driver scan. None of these fixtures constructs a full
link or exported client transaction. Link destruction, Format, buffer
replacement, input-port addition, and exported detach acknowledgment remain
to be exercised.
