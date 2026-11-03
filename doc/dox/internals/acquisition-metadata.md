\page page_acquisition_metadata Buffer metadata

# Buffer metadata

## Independent SPA metadata records

The native 4.0 source cohort simplifies the former native 3.0 records. This is a
coordinated breaking change across native code, bindings, algorithms, recording
and GUI. Source validation is recorded below; publication and deployment remain separate.
The former event-array/clock-mapping contract is retired; its history is in Git.

Use standard `SPA_META_Header` for flags, PTS and sequence. Additional facts are
small, independently optional SPA records. There is no per-buffer event history,
clock-mapping UUID, uncertainty, origin array or PTP requirement.

| Record | Native fields | Size |
| --- | --- | ---: |
| AcquisitionIdentity (0x300) | existing flags/reserved, domain, generation, sequence | 40 |
| ExposureTiming (0x301) | flags, clock, start_nsec (int64), duration_nsec (uint64) | 24 |
| ReferenceTimestamp (0x305) | clock, reserved, timestamp_nsec (uint64), duration_nsec (uint64) | 24 |
| OpticalChopping (0x303) | existing flags, classification | 8 |
| ElectricalTrigger (0x304) | existing flags, classification | 8 |

`AbsoluteTime` (0x302) and its event array are retired. No compatibility alias,
old-event reader or alternate schema selector belongs in the replacement APIs.
The changed ExposureTiming layout requires exact-size negotiation and a native 4.x
ABI cohort. Incompatible record layouts fail admission; new Julia clients also
reject a native library outside 4.x at startup. Header-only compatibility is not
a claim that different scientific metadata layouts are interchangeable.

ExposureTiming keeps only START_VALID and DURATION_VALID flags. A valid start
requires its clock; an absent start has zero start/clock. A present exposure
duration is positive. All-zero storage means no exposure fact. Start/PTS are
stream/model times, not implicitly wall-clock readings.

ReferenceTimestamp follows the small-value model of GstReferenceTimestampMeta:
one clock reference, one unsigned 64-bit nanosecond timestamp, and an optional
duration. Clock values retain MONOTONIC=1, REALTIME=2, TAI=3, MODEL=4. Clock=0
and all-zero storage mean unavailable; a populated record has reserved=0.
UINT64_MAX denotes an unspecified reference duration, and is not a valid timestamp.
REALTIME and TAI use their clock's Unix-epoch reference; a clock label makes no
cross-host synchronization or accuracy claim. Model time stays separate.

A source uses a hardware/source timestamp when available, otherwise a host clock
reading when it produces the sample. This timestamp is associated with the sample;
it is not an inferred exposure-start conversion. Pass-through preserves it.
A newly measured/applied sample may replace it explicitly. Derived outputs retain
the reference timestamp of their triggering input; there is no timestamp merge or
history append. Multi-camera contributor identities remain separate controller data.
Recording receipt/write times belong to the recorder, not to the buffer record.

AcquisitionIdentity remains required on ports whose chopping, stale-frame or
feedback consumers use original acquisitions. Source domain, generation and
sequence retain their existing authority/reset semantics; Header sequence alone
does not replace them across derived publications. Generic streams do not require
this record merely to transport an array.

Optical chopping describes fringed/unfringed/mixed images. Electrical trigger
describes low/high/mixed levels. They are independent; one does not imply the other.
Legacy explicit ABSENT values can remain on actual records where required by an
existing consumer, but irrelevant records are not negotiated by default.

## Allocation and propagation

Negotiate only the records a port produces or consumes. Required records must be
present and valid; unknown optional records remain opaque and can pass through.
Keep the existing bounded negotiated SPA descriptor array and preallocated payload
storage. Populate payloads per frame without allocation; do not resize or rewrite
exported descriptor arrays during processing. Initialize owned slots on reuse so
stale facts cannot leak. Preserve borrowed data only for the buffer lease.

The normal GUI shows populated values: sequence, timestamp in nanoseconds, clock,
exposure and applicable chopping/trigger state. It does not show event indices,
allocation status, type IDs, record byte sizes, empty fields or duplicated Header
values. Raw descriptors and acquisition provenance belong in a collapsed details
view. Browser-facing timestamps use decimal strings so JavaScript cannot round a
64-bit nanosecond value.

## Migration and verification

1. Native layouts, validation, propagation and capture encoding; generated bindings.
2. Julia/Rust bindings and transport-neutral values, without old-event aliases.
3. HIL producers, graph ingress/egress and calibration recording.
4. GUI snapshots and shared native/browser presentation.
5. Focused unit tests, bounded private-core roundtrips, chopped SCC/dual-feedback
   regressions, recording roundtrip and stopped GUI rendering tests.

Check exact layouts, missing/unknown records, overflow, nanosecond precision,
reuse, reset, original-frame identity, unequal camera cadence and independence of
optical/electrical state. Never use a desktop core or start a simulator/GUI for
validation. Keep temporary SDKs, targets and logs only until concise results are
recorded here.

## Source validation (2026-11-03)

Native changes start at `87436f1bd`; PipeWireAO.jl changes start at `046b948`.
Native/JLL 4.0 and PipeWireAO.jl 0.9 form the replacement cohort. Julia rejects
native libraries outside 4.x; layout admission rejects retired 0x302, its feature
forms and the former 64-byte ExposureTiming even when optional.

A scratch SDK was built with Meson, tests/examples/SPA plugins enabled, other
optional features disabled, and `ninja -j6`. Clang.jl 0.19 regenerated the Julia
bindings from its installed headers. Ten focused native gates passed: buffer
record validation, negotiation, metadata admission, callback buffer access,
FGN branching/retained records, three private-core metadata/mismatch cases, and
`test-spa`. Julia 1.13.3 focused metadata checks passed 92/92; the private-core
callback check passed 18/18. The complete PipeWireAO.jl suite passed all 66 named testsets (2,137 checks,
including all 11 Aqua checks) and the SDK load guards, using a validation environment with local SDK
preferences and `JULIA_LOAD_PATH=@:<validation environment>:@stdlib`. These exercise exact layouts, integer overflow and
precision, invalid/absent records, opaque snapshots, reuse, independent optical
and electrical facts and zero warmed callback allocations. No device, desktop
core or deployed application was used.

The maintained optional `PIPEWIREAO_TEST_RETIRED_PREFIX` subprocess check passed
2/2 against native 3.0: package initialization rejected the retired cohort before
any buffer layout was read, while the active SDK remained native 4.0.

AdaptiveOpticsSimPipeWireHIL 0.5 passed its full ordered Julia 1.13.3 suite:
3,450/3,450 checks, including model-time capture, independent source profiles,
exact reference propagation, four zero-allocation optional-presence cases,
private-core publication and Float32/UInt16 held/named calibration. The new
optional reference exposed an eight-combination dynamic adoption call: an
allocation trace attributed 160 bytes to call argument boxing. Inlining the two
existing adoption call sites removed that boxing without changing receipt storage
or weakening the allocation checks.
The final mixed optional-reference model-time regression passed 22 checks; its
fixed provenance type preserves replay when reference presence changes.

Independent source review found no blocking issues. RTC core checks passed
10,295/10,295; recording/callback checks passed 229 and 126; explicit profiles and
private command/named transport checks passed. JFG's full suite and focused SCC,
composition and allocation checks passed. A stale helper found at the end of the
FGP broad run was removed and its focused selector passed; that broad run was not
repeated. Native/browser GUI suites, UInt64 string roundtrips and Rust wrappers
passed. Queue COPY/LEASE, Header-only, opaque and required-record tests passed.
Strict GUI Clippy retains six pre-existing compiler-lint findings outside this
change; the scoped check passed. Installed applications and device calibration
bytes were not changed.

Final queue export regression passed 140 focused checks. The failing source
version omitted SCC classification slots from observation queues; the replacement
uses resolved graph/simulator profiles and session links. Queue chains preserve
explicit upstream classifications; incompatible authored downstream layouts reject.
Independent review confirmed the propagation fix. Ordinary LLOWFS and unconnected
queues retain the smaller profile.

The regenerated offline SPIDERS application passed 91/91 private-core command
handoff checks: all 16 commands were consumed, with zero reported drops,
rejections or gaps. Camera, controller and observer metadata profiles agree;
test observers now request the exact output profile, as the production GUI does.
This is synthetic-pixel CPU transport validation, not optical or deadline
qualification. The offline export has no deployable registered dependency closure;
no installed application, simulator service or GUI was started or updated.
Temporary SDKs, source-validation environments, builds and logs were removed
after recording these results.

Representative source-validation commands (the validation environments select
the scratch SDK and unpublished source cohort):

```sh
meson setup "$build" --prefix="$prefix" --libdir=lib -Dauto_features=disabled \
  -Dtests=enabled -Dexamples=enabled -Dsession-managers=[] -Dspa-plugins=enabled
ninja -C "$build" -j6
ninja -C "$build" install
PIPEWIREAO_INCLUDE_ROOT="$prefix/include" julia --project="$gen_env" gen/generator.jl
JULIA_LOAD_PATH="@:$validation_env:@stdlib" julia --project="$validation_env" --threads=2,1 test/runtests.jl
PIPEWIREAO_EXCHANGE_TEST_PREFIX="$prefix" julia --project="$validation_env" --threads=2,1 test/metadata_records_private_core.jl
PIPEWIREAO_HIL_TEST_PREFIX="$prefix" JULIA_LOAD_PATH="@:$hil_env:@stdlib" julia --project="$hil_env" --threads=2,1 test/runtests.jl
```

## References

- [SPA buffer layout](https://docs.pipewire.org/page_spa_buffer.html)
- [SPA Header](https://docs.pipewire.org/structspa__meta__header.html)
- [GstReferenceTimestampMeta](https://gstreamer.freedesktop.org/documentation/gstreamer/gstbuffer.html#gst_buffer_add_reference_timestamp_meta)
- [GNU Radio 4 tags](https://github.com/fair-acc/gnuradio4/blob/main/core/include/gnuradio-4.0/Tag.hpp)
