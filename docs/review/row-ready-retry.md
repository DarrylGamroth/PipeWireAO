# Rejected row-ready activation

Base: `1045d32aedcd800783c69e984ac62de22d6187f8`.
Worktree: `pipewire-row-ready-retry`, branch `fix-row-ready-retry`.

## Observed failure

The held live replay `rtc-copper-julia-live-updates-ready-return-1024-r2-20260929`
recorded source frame 253, buffer 1, generation 64 returning `-EBUSY` from ready.
No later source ready or return record followed. All active core nodes finished,
the core retry latch was clear, and Julia received no live Parameter callback.

## Repair

- Retain rejected row-driver ready in the existing atomic retry latch and signal
  its driver-loop event. Reset the dispatched flag so a repeated rejection can
  rearm the same request. The event also covers completion before the latch write.
- Defer retry dispatch while a new source row overlaps a borrowed prior row.
  Retained-input control retries without a new source row remain permitted.
- Clear pending retry on unprepare beside the existing cycle/dispatched resets.
  Borrowed-row release ownership remains unchanged. The source owns retained
  unaccepted publication across stop/start and supplies its next ready activation.

No public interface, timer, main-thread node event, or normal-driver admission
behavior changes.

## Validation

Original logs and exact commands are in [row-ready-retry-evidence](row-ready-retry-evidence/validation.json).

- Deterministic old-driver-finished/async-publisher-awake regression fails before
  repair at `reliable_retry_pending == 1`, then passes after repair.
- Repeated rejection, overlapping borrowed/new source row, single dispatch,
  unchanged source buffer ID, and accepted next-cycle admission pass.
- Actual unprepare/prepare regression fails before the pending reset, then passes.
  It preserves a pending release and emits no stale command after prepare.
- Existing retained-input retry and concurrent release-order tests pass.
- Normal candidate DSO builds without warnings using one isolated replacement
  object and 36 unchanged objects; the original DSO hash is unchanged.

The fixture invokes the completion event explicitly. It proves latch and dispatch
behavior; combined source/core live replay must establish production completion
wake integration and end-to-end timing. No installation or commit was performed.

Independent Astra review on 2026-09-29 approved the core candidate for combined
runtime validation. The unprepare/prepare regression closes review item CF10-R1;
no additional core changes were requested.
