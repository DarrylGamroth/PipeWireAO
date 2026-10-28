# Upstream rebase, 2026-10-28

## Source and result

- Original local master: `e641c398a32ad7c77613592fbb43805430ef42ec`.
- Backup branch: `backup/pre-upstream-rebase-20261028`.
- Previous upstream base: `d80b80602b389fe9badeda830122d357e9e2e3ea`.
- New upstream base: `be282a9c4` (`origin/master`, fetched 2026-10-28).
- Integration branch: `work/upstream-rebase-20261028`.
- Validation worktree: `../pipewire-upstream-rebase-20261028`.
- Original working tree was clean.

Replayed 212 first-parent commits onto the new upstream base. The original
merge `3d9537dc7` was flattened to its resolved delta against its first parent;
its side branch was not replayed as a second copy of the fork history.

## Integration decisions

- Preserve upstream `output_mode` and per-port mix method tables.
- Use upstream `PW_BUFFERS_FLAG_DELAYED` for reliable output allocation.
  Async and delayed recycling each reserve one buffer; their combination
  reserves three buffers when the base minimum is one.
- Preserve upstream ordinary-link buffer-ID exchange and the fork's explicit
  row-return invalidation, borrow tracking, and retry handling.
- Preserve upstream's separate activation-state reset pass.
- Keep current upstream CI, audio, and V4L2 changes when the old merge carries
  earlier versions of those changes.
- Preserve the fork's project participation instructions.

The five ndarray files that previously produced add/add conflicts are
unchanged from the original master: `filter-graph-ndarray.c`,
`plugin_ndarray_example.c`, `test-filter-graph-ndarray.c`,
`module-ndarray-filter-chain.c`, and `ndarray-filter.c`.

## Verification

Configured `build-rebase` with `debugoptimized`, tests enabled, and the same
AO-focused disabled audio/video conversion features as the existing build.

- `ninja -C build-rebase -j 8`: passed, no compiler warnings.
- `meson test -C build-rebase --print-errorlogs`: 72 passed; no failures,
  skips, or timeouts.
- Buffer negotiation test: 256 cases, including synchronous delayed and
  asynchronous delayed reserves.
- Ordinary reliable-link regression: four handoffs return consumed IDs through
  shared IO without explicit row-release callbacks; targeted test passed.
- Independent source review of node, port, link, allocation, and row-return
  integration: no confirmed defect.
- `git diff --check`: passed.
- Built daemon reports compiled and linked library version `1.7.1`.

## Deployment boundary

This operation updates local source history. `/opt/pipewireao` still reports
`1.7.0`. The Julia JLL remains `1.7.1+0`, built from `62a3aff80`; the local
`PipeWireAO.jl` manifest selects that JLL. Neither installation nor artifact
publication is part of this rebase. No remote branch was pushed.
