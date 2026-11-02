\page page_acquisition_metadata Acquisition identity and multi-host timing

# Acquisition identity and multi-host timing

## Status and authority

Planning update, 2026-11-02: [independent SPA metadata records](#planned-independent-spa-metadata-records)
defines the next migration. It is a proposed target, not an implemented ABI.
The native 2.0 Properties contract below remains the released behavior until
the coordinated replacement passes its delivery gates. No runtime changes are
authorized by this documentation-only planning update.

This document defines the PipeWireAO acquisition identity, timing, and join
contract. It applies to complete frames and to complete row-block ndarray
micro-buffers. It does not make the PipeWire scheduler a semantic join engine.

The key words **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**, **SHALL NOT**,
**SHOULD**, **SHOULD NOT**, **RECOMMENDED**, **NOT RECOMMENDED**, **MAY**, and
**OPTIONAL** in this document are to be interpreted as described in BCP 14
(RFC 2119 and RFC 8174) when, and only when, they appear in all capitals.

`spa/include/spa/buffer/meta.h` is the ABI authority. This document is the
semantic authority. A transport profile defines its own encoding and carriage,
but it MUST NOT change these field meanings.

## System boundary

PipeWire graph scheduling answers when a runnable node may process. It does not
decide whether buffers from independent cameras describe the same physical
acquisition. That decision belongs to a bounded semantic join.

```mermaid
flowchart LR
    CameraA[Camera on host A] --> SourceA[Source A]
    CameraB[Camera on host B] --> SourceB[Source B]
    SourceA --> MetaA[Header and buffer Properties]
    SourceB --> MetaB[Header and buffer Properties]
    MetaA --> SendAdapter[Transport send adapter]
    SendAdapter --> Transport[Network transport]
    Transport --> ReceiveAdapter[Transport receive adapter]
    ReceiveAdapter --> Join[Bounded semantic join]
    MetaB --> Join
    Join --> Algorithm[Multi-input algorithm]
```

The same contract applies when both sources are local; the encode, transport,
and decode steps are then absent.

## Identity contract

The acquisition identity is the exact tuple:

```text
(domain, generation, sequence)
```

- `domain` is a nonzero, opaque 128-bit identifier for one acquisition or
  trigger authority.
- `generation` is a 64-bit epoch assigned by that authority.
- `sequence` is the 64-bit acquisition number within that generation.

**ACQ-ID-001:** Producers that claim a shared identity domain **MUST** receive
`domain` and `generation` from the same control-plane authority and **MUST**
derive `sequence` from the same physical trigger or acquisition counter.

Verification: configure two simulated hosts with the same authority values and
prove that equal physical acquisitions compare equal while a changed domain,
generation, or sequence compares unequal.

**ACQ-ID-002:** A producer **MUST NOT** advance a shared generation because of
its own pause, restart, packet loss, or local process lifecycle. Before a
sequence can reset, wrap, or be reused, the authority **MUST** distribute a new
generation to every participant. If the producer observes a non-increasing
sequence for a newly completed physical acquisition without that reconfiguration,
it **MUST** fail closed and stop publishing valid identity. Re-publication of a
retained acquisition preserves its identity; a new publication Header does not
claim a new physical acquisition.

Verification: inject a duplicate or reset sequence and prove that the producer
does not publish a new locally invented identity tuple.

Exact identity is independent of clock synchronization and is the preferred
join key. A PTP timestamp is a fallback for sources without a common hardware
counter and a consistency check for sources that have one.

## Per-buffer representation

Native 2.0 implements this representation. The preceding 1.7.3 cohort used
the superseded Acquisition record. The replacement is standard `SPA_META_Header`
and the existing `SPA_META_Properties` BufferProperties Object, version 1.
Metadata IDs 11 and 12 remain retired and MUST NOT be reused. No Acquisition
version, compatibility reader, or version-selection flag remains in the new
runtime cohort. This changes the ndarray callback ABI and requires a coordinated
native/bindings/application release; it is not an SDK replacement for old clients.

The native Header retains publication sequence, presentation time and lifecycle
flags. Acquisition identity and exposure facts are independent Properties.
A held sample retains its acquisition tuple while each publication advances
Header sequence and presentation time. Equality of these sequence numbers is
not a general metadata invariant. Header PTS MUST NOT substitute for physical
exposure start.

The following keys extend the existing Object without changing existing keys or
the Object version. POD Bytes containing an unsigned integer use exactly eight
little-endian bytes, covering the entire UInt64 range. They are values inside a
native POD Object, not a cross-host encoding of the whole Object.

| Key | Name | POD type | Meaning |
| --- | --- | --- | --- |
| 1 | `opticalClassification` | Id | Independent absent/unfringed/fringed/mixed optical fact, values 0/1/2/3 |
| 2 | `triggerClassification` | Id | Independent absent/low/high/mixed electrical fact, values 0/1/2/3 |
| 3 | `acquisitionDomain` | Bytes16 | Nonzero acquisition-authority identifier |
| 4 | `acquisitionGeneration` | Bytes8 | Full unsigned authority generation |
| 5 | `acquisitionSequence` | Bytes8 | Full unsigned physical acquisition counter |
| 6 | `exposureDuration` | Bytes8 | Positive duration in nanoseconds |
| 7 | `exposureStart` | Long | Nonnegative physical exposure start in nanoseconds |
| 8 | `exposureTimebase` | Id | MONOTONIC=1 or TAI=2 |
| 9 | `timestampUncertainty` | Bytes8 | Inclusive unsigned timestamp error bound in nanoseconds |
| 10 | `ptpGrandmasterIdentity` | Bytes8 | Nonzero IEEE 1588 clock identity, preserving byte order |
| 11 | `ptpDomainNumber` | Id | IEEE 1588 domain, 0 through 255 |

**ACQ-ABI-001:** A producer MUST initialize every reused Properties allocation
before adding facts. A consumer MUST validate the complete Object before
interpreting it. Properties retain their existing 16–4096-byte, eight-byte-aligned,
64-field bounds, strict key order, exact types/sizes, zero padding and tail rules.
The ndarray aggregate metadata budget remains 4096 bytes, including Header.

**ACQ-PROP-001:** Keys 3–11 MUST carry `SPA_POD_PROP_FLAG_MANDATORY` when
present. Unknown well-formed optional keys MUST remain skippable. Unknown
mandatory keys MUST fail admission unless a consumer requirement declares
understanding. Each consumer MUST declare the facts and exact POD types it
requires; absent required keys and wrong types/sizes MUST fail before use.
Container-version support alone does not promise a particular fact is present.

**ACQ-PROP-002:** Identity keys 3–5 MUST appear together or all be absent.
Exposure-start keys 7–9 MUST appear together or all be absent. Duration is
independent. TAI additionally requires keys 10–11 with a nonzero grandmaster;
MONOTONIC MUST NOT include PTP keys. PTP keys without a start are invalid.
Classification absence differs from a present absent-classification value.
Partial groups and invalid ranges MUST fail complete-record validation.
Private builders may append a group incrementally; incomplete objects MUST NOT
be published or accepted by readers. Append helpers perform structural checks
while complete validation also checks semantic groups.

**ACQ-PROP-003:** Unchanged pass-through MUST preserve all validated Properties,
including unknown optional fields, under the original buffer lease. Derived
outputs MUST use explicit scientific provenance; a sum MUST NOT invent one
camera's identity for the combined command. Output reuse MUST clear stale facts.
Header publication identity and retained acquisition identity MUST remain distinct.

**ACQ-TRIGGER-001:** Trigger Id values classify the electrical state over the
whole exposure. Optical classification MUST NOT be inferred from electrical
polarity by a generic transport. An optical application relating them MUST supply
its explicit mapping, including absent and mixed states.

**ACQ-TRIGGER-002:** Chopped pairing MUST require complete acquisition identity,
positive duration and explicit usable optical classification. It MUST validate
same authority/generation, adjacent acquisition counters, compatible duration,
and alternating fringed/unfringed roles. Duplicates, stale records and gaps
MUST follow the existing bounded SCC reset/rejection policy, independently of
Header publication counters. Missing metadata MUST NOT be reconstructed from
arrival order or from a missing property treated as zero.

**ACQ-TRIGGER-003:** The new cohort MUST neither negotiate nor publish the retired
metadata type 11. Old-schema-only peers MUST fail required-Properties negotiation
or semantic admission, rather than silently running with missing identity/timing.

Verification: full-range unsigned round trips; malformed types/sizes/flags,
partial groups and padding; buffer reuse; required-key rejection; held-sample
publication/acquisition divergence; all electrical/optical classifications;
and chopped loss/restart/duplicate tests. Native callbacks expose borrowed
Properties and Header, without the old by-value Acquisition record.

### Public native helper vocabulary

`spa_acquisition_identity` and `spa_exposure_start` are owned semantic values,
not separately versioned wire records. `spa_meta_properties_get_acquisition_identity`,
`spa_meta_properties_get_exposure_start` and
`spa_meta_properties_get_exposure_duration` read validated facts.
`spa_meta_properties_add` accepts explicit property flags;
`spa_meta_properties_add_uint64` and `spa_meta_properties_get_uint64` implement
the eight-byte unsigned representation. `spa_acquisition_identity_equal`,
`spa_exposure_start_time_difference` and `spa_exposure_start_times_match` preserve
the identity, clock and saturating comparison rules below. No old-name aliases
are introduced.

## PTP-qualified time

A cross-host exposure timestamp has all of these properties:

- `exposureStart` is the physical exposure start expressed as
  nonnegative Linux `CLOCK_TAI` nanoseconds;
- `timebase` is `TAI`;
- the complete exposure-start and PTP property groups are present;
- `ptpGrandmasterIdentity` is the nonzero IEEE 1588 grandmaster clock identity;
- `ptpDomainNumber` is the IEEE 1588 domain number; and
- `timestampUncertainty` is a conservative inclusive error bound.

**ACQ-TIME-001:** A producer **MUST** use
the TAI exposure-start property group only when the device timestamp
is traceably mapped to `CLOCK_TAI`. The mapping **MUST** account for the PTP
timescale and UTC offset. A disciplined `CLOCK_MONOTONIC`, host arrival time,
buffer completion time, or unqualified device tick is not a substitute.

Verification: compare the reported exposure time against a qualified hardware
timestamp source while exercising the configured PTP profile.

**ACQ-TIME-002:** `timestampUncertainty` **MUST** bound device timestamp
resolution, exposure-latch error, device-to-PHC conversion error, PHC-to-TAI
correlation error, and the accepted synchronization error. If no defensible
bound is available, the producer **MUST NOT** publish a PTP-qualified group.

Verification: measure each error term or obtain a guaranteed bound, record the
sum used by the producer, and test the bound during steady state and recovery.

**ACQ-TIME-003:** After loss of synchronization, an unbounded clock step, an
unknown UTC offset, or an unqualified grandmaster change, the producer **MUST**
stop publishing PTP-valid time and **SHOULD** mark the next recovered output
`SPA_META_HEADER_FLAG_DISCONT`.

Verification: inject each loss condition and prove that no PTP-valid record is
published until qualification recovers.

Two exposure-start values are comparable only when both records validate, both
are PTP-qualified TAI values, and their grandmaster identities and PTP domain
numbers are byte-for-byte equal. The helper
`spa_exposure_start_time_difference()` enforces these preconditions and
returns:

```text
difference = exposure_start_a - exposure_start_b
combined_uncertainty = saturating_add(uncertainty_a, uncertainty_b)
```

Host location, clock servo name, and similar wall-clock readings are not part
of the comparison.

## Join contract

**ACQ-JOIN-001:** A join **SHOULD** match an exact acquisition identity first.
It **MAY** use PTP time when identity is absent, or use it to reject an
identity match that violates a configured physical-time bound.

**ACQ-JOIN-002:** A time-based join **MUST** choose an application-specific
additional tolerance `T` and may match only when:

```text
abs(difference) <= combined_uncertainty + T
```

The addition is saturating. `spa_exposure_start_times_match()` implements
this test. `T` does not replace the reported uncertainty and **SHOULD** be less
than half the minimum acquisition interval so adjacent acquisitions cannot
match under nominal conditions.

**ACQ-JOIN-003:** A join **MUST** have bounded per-input storage, a deadline,
and an explicit policy for missing, late, duplicate, and ambiguous inputs. The
policy may drop, hold the last value, or emit an incomplete result, but a
non-real-time input **MUST NOT** retain an unbounded number of RTC buffers.

Verification: cover exact matches, different identity fields, matching and
different PTP authorities, the uncertainty boundary, adjacent acquisitions,
duplicates, missing inputs, reordering, deadline release, and overload.

## Transport boundary

SPA metadata is native shared-memory state. PipeWire does not serialize it
across hosts, and an application MUST NOT send the bytes of the native C
structure as a network protocol. The network adapter is the correct owner of
transport versioning, byte order, framing, authentication, and association
with fragmented payload data. No transport codec is exposed by the core SPA
metadata API until a concrete transport profile requires one.

**ACQ-WIRE-001:** A cross-host adapter **MUST** carry one acquisition record with
the corresponding payload according to its transport profile and **MUST**
preserve their association through loss, reordering, duplication, and
fragmentation. It **MUST NOT** copy the native C structure directly onto the
wire.

**ACQ-WIRE-002:** A receiving adapter **MUST** decode the transport record,
construct native metadata through the public Properties helpers, and validate
it before publishing local Header and Properties. Invalid or missing records
**MUST NOT** be synthesized from packet arrival time.

Verification belongs to the selected transport implementation. It must include
golden vectors and round trips on both endian architectures where available,
then exercise loss, reordering, duplication, fragmentation, and malformed
records.

## Capability and migration gates

Hardware PTP qualification and cross-host codecs are not established by local
SPA transport tests. Device adapters must migrate their producers separately
before using the new runtime cohort; missing facts cannot be manufactured.

| Gate | Required evidence | Status |
| --- | --- | --- |
| Semantic and native contract | Identity/range/clock/hold rules and additive keys reviewed | Reviewed; native boundary independently reviewed |
| Native transport and queues | No retired record IDs; private-core Properties preservation, mismatch/reuse/fanout tests | Native focused tests and 17 queue tests pass |
| Julia and Rust bindings | Regenerated ABI; no version selection or legacy transport readers | Rust and Julia unit/private-core tests pass; regenerated ABI reviewed |
| Scientific algorithms and HIL | Version-free chopping, joins, commands and held identities; zero-allocation warmed transport | Semantic and private-core tests pass, including required extensions, held/captured retention and warmed zero allocation |
| RTC, capture, GUI and applications | New-schema-only source→queue→RTC→DM→GUI/capture, cold schemas and pins updated | Capture and GUI tests pass; all four instrument cold exports pass |
| Retirement | Independent review, old-schema rejection, full dependency-cohort validation and release | Independent review complete; packaged native tests pass; publish only the complete staged cohort |

## Delivery plan and identifier preflight

Native baseline `15525f80f`, PipeWireAO.jl `687d420`, JFG `68df2f5`, HIL `d676abc`,
RTC `f7923eb`, Rust `e362a2c`, queue plugins `975c553`, GUI `a7ea558`, and instrument
monorepo `37dc272` are isolated on `codex/metadata-migration` worktrees. Pre-existing
checkouts and running unrelated sessions remain untouched. Simulators and GUI
for this work remain stopped; integration uses private cores and bounded fixtures.

The dependency order is native helpers/callback ABI and queues, regenerated
Julia/Rust bindings, scientific adapters and HIL, then RTC/GUI/capture and authored
instrument configuration. Only a complete validated replacement cohort is
published; no v2 intermediate is a delivery. Frozen scientific calibration bytes
and historical capture bytes are not hand-edited. Raw logs/builds are temporary.

The following flow preserves publication and acquisition as separate facts:

```mermaid
flowchart LR
    Source["Acquisition authority"] --> Facts["Identity, exposure and classification Properties"]
    Header["Publication Header"] --> Buffer["One buffer lease"]
    Facts --> Buffer
    Buffer --> Queue["Queue or held sample"]
    Queue --> SCC["SCC pairing by acquisition identity"]
    Queue --> LL["LLOWFS measurements"]
    SCC --> DM["Explicit shared-command provenance"]
    LL --> DM
```

| Vocabulary | Authority and scope |
| --- | --- |
| Header, Properties, POD Id/Long/Bytes, mandatory flags | Adopted SPA metadata/POD boundary; existing Object contract |
| acquisition domain/generation/sequence | ACQ-ID-001/002; full-width physical authority identity |
| exposure start/duration, uncertainty, PTP grandmaster/domain | ACQ-TIME-001/002/003 and existing IEEE 1588 qualification semantics |
| optical/trigger classifications | Existing independent key 1/2 contract; no inferred mapping |
| publication sequence | Standard Header plus established held-sample contract; not an acquisition alias |

Existing ACQ-ID/TIME/JOIN/WIRE requirement identities and meanings remain intact.
ACQ-ABI and TRIGGER clauses now define the replacement boundary; ACQ-PROP clauses
make extensibility and complete semantic groups explicit. The native header is
implemented against this authority, not used to invent competing terminology.

The complete software cohort is native/JLL 2.0.0, PipeWireAO.jl 0.7.0,
FGA 0.9.0, JFG 0.5.0, FGPW/FI 0.2.0, HIL/RTC 0.3.0. Independent review
findings on required-field admission and opaque-field retention were reproduced,
fixed and reviewed again. Packaged native validation passes 171 binding and
1,632 HIL calibration assertions without SDK preferences. ARM build/hash/header
checks do not establish ARM runtime qualification.

## Planned independent SPA metadata records

### Decision and applicability

Target design, 2026-11-02; implementation has not started. This section owns
the framework-wide metadata migration plan for every PipeWireAO RTC, device
adapter, simulator, algorithm executor, observer and recorder. SPIDERS and CAL2
are acceptance applications, not definitions of the generic boundary.

Replace acquisition facts inside the single BufferProperties Object with
independently typed `spa_meta` entries alongside standard `SPA_META_Header`.
Add a new metadata type to add a new semantic capability; do not extend a
universal acquisition record or introduce a global acquisition version selector.
Absolute REALTIME or TAI timestamps are part of telemetry support. PTP is not
a dependency of the base framework; a future clock provider may qualify clocks.

The requirements below are target obligations using the BCP 14 convention
declared above. They do not claim current conformance. Earlier ACQ-ID identity
rules remain applicable. Existing ACQ-ABI/PROP/TRIGGER/TIME/JOIN/WIRE clauses describe
the released cohort; phase 1 must reconcile them explicitly, retaining their
IDs and history rather than silently assigning them different meanings.

Non-goals: implementing a network transport, clock discipline, hardware PTP,
new scientific estimators, new controller authority, or physical/deadline
qualification. Keep simulation and GUI sessions stopped during this migration;
private-core integration tests own and close their cores.

### Record boundaries and terminology

These are semantic record descriptions, not assigned C type names or numeric
IDs. Native headers will own the final ABI and generated bindings will follow.

| Record | Coherent contents and meaning | Applicability |
| --- | --- | --- |
| Standard SPA Header | Existing presentation timestamp in ns, sequence, flags and decoding offset; PTS alone does not declare an absolute epoch or clock | Base framework stream profile |
| Acquisition identity | Existing nonzero authority domain, full UInt64 generation and sequence; retained exposures keep their identity | Acquisition products and consumers needing association |
| Exposure timing | Exposure interval, stated start/end convention, units and clock domain; source timing is distinct from queue arrival | Integrating sensors and algorithms needing exposure coverage |
| Absolute time | Integer timestamp, REALTIME or TAI basis, event meaning, validity and provenance of any clock mapping | Recorded telemetry; optional on other ports |
| Optical chopping | Whole-exposure absent/unfringed/fringed/mixed optical classification, independent of electrical state | Chopped optical producers and consumers |
| Electrical trigger | Existing absent/low/high/mixed whole-exposure electrical classification | Trigger-aware producers and consumers |

Split by semantic unit, not one metadata type per scalar. Do not put unrelated
instrument settings, calibration arrays, node properties or recording filenames
in these records. Existing product/command provenance stays explicit; combined
outputs cannot be represented by arbitrarily selecting one camera's identity.
Phase 1 inventories existing contributor/feedback lineage before assigning ABI
layouts; it must preserve those semantics without inventing an unbounded history.

### Planned port and buffer contract

**ACQ-META-001:** Each supported framework stream MUST carry standard Header
metadata. Additional record types MUST be independently negotiable. Producers
MUST NOT represent the target acquisition facts through the released Properties
key groups or an Acquisition v1/v2/v3 compatibility record.

**ACQ-META-002:** A port MUST declare required record types/layouts and the
optional types it can supply or accept. Preparation MUST reject an unsatisfied
required capability before admission. A consumer MUST validate required records
on every buffer; negotiated allocation alone MUST NOT imply valid information.
Missing/invalid required facts MUST reject that input before scientific use.
Application policy owns hold/drop/fault behavior; rejection must not publish an
apparently valid result or partially advance scientific state.

**ACQ-META-003:** Buffer pools MUST allocate bounded record storage during
preparation. Initial limits are at most 64 metadata entries and 4096 aggregate
metadata bytes, including Header, alignment and feature entries, for the ndarray
profile. Other profiles MUST state finite limits explicitly. Adding a type or
increasing capacity beyond the negotiated layout MUST require a cold pool
replacement/renegotiation, not mutation of a borrowed buffer skeleton.

**ACQ-META-004:** A producer MUST initialize each owned output record on reuse
and explicitly mark unavailable facts invalid. Missing metadata, allocated but
invalid metadata, and a valid explicit absent-classification value MUST remain
distinguishable. Each proposed record layout MUST define its validity encoding,
reserved-byte rules and validation before bindings are generated. Duplicate
instances of a singleton record MUST be rejected.

**ACQ-META-005:** Published metadata MUST remain immutable for its buffer lease.
Only the owner of a writable output may populate or invalidate its records.
Consumers MUST NOT retain borrowed views beyond the callback/lease. Fanout
MUST NOT permit one consumer to remove or mutate another consumer's records.

**ACQ-META-006:** A node MUST declare record propagation as preserve, produce,
replace or drop. Unchanged pass-through MUST preserve accepted unknown optional
records byte-for-byte when their negotiated storage is available. A transform
MUST preserve only facts that remain true and MUST explicitly describe derived
output provenance. Required downstream records that would be dropped or cannot
fit MUST fail preparation; unexpected runtime overflow MUST reject publication,
without truncation or silent fallback. All paths MUST clear stale output facts.

**ACQ-META-007:** Required types/layouts MUST be supported explicitly; unknown
optional types MUST be skippable within negotiated bounds. Generic native
validation MUST check envelope/count/size/alignment bounds; validation of an
understood type MUST additionally check its flags, validity and semantic rules.
Unknown optional payloads remain opaque bytes under declared pass-through. Core
iteration MUST NOT depend on a hard-coded switch over every possible type.
Incompatible evolution MUST affect the specific type/layout, not every record.

**ACQ-META-008:** Optical and electrical classifications MUST remain independent.
Only a producer with an explicitly configured physical mapping may derive
optical classification from an electrical observation. Mixed-exposure and
missing/invalid chopping cases MUST NOT be treated as ordinary unfringed input.
Existing full-width identity, duplicate, generation and held-publication rules
MUST remain intact through the change.

Adding/removing valid information means populating or invalidating negotiated
record slots. It does not mean changing `n_metas`, sizes or pointers per frame.
Nodes producing a different metadata set use their own negotiated output pool.

### Timing and recording

**ACQ-META-TIME-001:** Standard Header PTS MUST retain its presentation-timeline
meaning. Neither acquisition identity nor physical event time may be inferred
from it. The stream profile MUST identify its PTS clock domain/epoch or declare
that absolute conversion is unavailable. Host scheduling/deadline measurements
MUST use a suitable monotonic clock independently of absolute recording time.

**ACQ-META-TIME-002:** The framework MUST support absolute-time records in
REALTIME and TAI, with integer seconds/nanoseconds or a checked Int64 nanosecond
representation, an explicit epoch/time scale and an explicit event meaning.
The native ABI design MUST choose one representation and verify its range and
cross-language layout before implementation. REALTIME means POSIX epoch time;
TAI means the system TAI time scale with its required offset correctly configured.
Neither label alone asserts synchronization or an accuracy bound. Reading the
clock MUST NOT pass through floating-point seconds.

**ACQ-META-TIME-003:** Telemetry recording profiles MUST require absolute time
for each recorded product and declare its event convention. REALTIME is the
default; TAI is an explicit selection. Acquisition, output production, receipt
and file-write time MUST NOT be interchangeable. A recorder MUST preserve source
event time and identity; any recorder observation time is separate provenance.
If source absolute time is unavailable, a profile may explicitly select recorder
receipt time, but MUST label that event and unavailable source time rather than
silently manufacture a source timestamp. Non-recording ports need only the
records required by their algorithms.

Recorder receive/write observations belong in its bounded recording index or
sidecar, not a mutation of the borrowed source buffer. Phase 1 must state each
record's multiplicity: acquisition identity, exposure interval and each
classification are singletons. Absolute-time event/origin multiplicity must be
bounded explicitly so preserving source time and describing a derived product's
own event never requires ambiguous duplicate singleton records. A reference to
source time in existing bounded provenance is allowed; silent replacement is not.

**ACQ-META-TIME-004:** A producer mapping a device/model/host clock to absolute
time MUST identify the mapping and its validity. It MUST NOT claim physical
acquisition timing from callback arrival. Explicit uncertainty is required when
a consumer relies on a quantitative accuracy bound. Clock steps, invalid TAI
configuration and invalid mappings MUST be observable; absolute clock jumps
MUST NOT reset acquisition identity, reorder acquisitions, or control deadlines.
Simulation/model time MUST remain distinguishable from execution wall time.

**ACQ-META-TIME-005:** Cross-clock comparisons MUST require the same identified
clock domain or an explicit applicable mapping with accuracy adequate for the
consumer's configured tolerance. REALTIME/TAI labels on different hosts MUST NOT
by themselves establish that condition. PTP-specific fields and clock discipline
MUST NOT be required by the base profile. A future PTP provider may add a separate
qualification record without modifying existing records. RTP/RTCP adapters own
their timestamp mapping; an RTP counter is not an absolute acquisition time.

### Ownership and flow

The producer owns event facts; the graph owns transformations; recording owns
storage and its own observations. The following diagram is informative.

```mermaid
flowchart LR
    Source["Device or simulator"] --> Pool["Prepared pool and independent records"]
    Pool --> Port["Required-record validation"]
    Port --> Node["Algorithm and declared propagation"]
    Node --> Output["Owned output pool"]
    Output --> Control["Controller or actuator"]
    Output --> Observe["GUI and recorder"]
    Clock["Clock provider"] --> Source
```

Observers must keep their existing bounded, non-gating ownership. Optional
recording does not gain control-path leases or new RTC lifecycle authority.

### Implementation order and gates

Baseline: native `8bccb6e22`, PipeWireAO.jl 0.7.1/native-JLL 2.0.0,
FGA 0.9.0, JFG 0.5.0, FGPW/FI 0.2.0, HIL/RTC 0.3.0. RTC main also contains
`b867976` settings/shutdown fixes, which have not been released. Scientific
AOS 0.3.18/AOC 0.17.3/Proper 0.4.1 and frozen calibrations remain unchanged.

| Phase | Owner and likely surfaces | Exit gate |
| --- | --- | --- |
| 1. ABI and negotiation | Native `spa/buffer/meta.h`, type descriptions, `src/pipewire/buffers.c`, ndarray port/helper API; reconcile this document and native traceability | Reviewed record layouts, validity, event/clock enums, compatible-size rules, type IDs, finite limits and port contracts; allocator proof for required custom types and optional opaque pass-through |
| 2. Native vertical slice | Native filters/queues and SPA plugin image helpers | Header + absolute time + optional identity/chopping through producer, queue, consumer and fanout; negative admission/reuse/overflow tests; unsupported old ABI rejected |
| 3. Language boundaries | Native JLL; PipeWireAO.jl generated `LibPipeWire`, buffer/stream/ndarray APIs; Rust sys/high-level bindings | Same golden layouts/bytes and failure semantics; borrowed lifetimes; private-core round trips; zero warmed metadata heap allocation |
| 4. Algorithms and sources | FGA, JFG/FGPW/FI, AOS/HIL adapters, device plugins and graph warmup/capture | Chopped pairing, independent sensor arrivals, held/captured samples, row-block paths, shared command lineage and reset preserved; source timestamp conventions documented |
| 5. RTC and observation | RTC owners/export/configuration/calibration capture; GUI native/browser bridge and FITS/recording paths | Required metadata resolves in cold schemas; both absolute clocks record correctly; clocks remain labelled; no lost/forged source provenance or unknown optional pass-through |
| 6. Applications and release | All supported RTC profiles, including explicit inventory/disposition of Copper's older cohort | Fresh CPU/captured-GPU applications, matching dependency cohort, capture/GUI schemas, independent review and complete release/pin update; retired compatibility paths removed |

Current allocator code represents required metadata with a 32-bit mask and
reserves low numeric IDs. Phase 1 must prove how custom types beyond that mask
are negotiated and required, retaining interoperability with upstream standard
types. Do not allocate more low standard slots as a workaround. Reserve retired
IDs 11/12 and the published Properties ID against incompatible reuse. Reuse
upstream mechanisms where sufficient; document any necessary AO extension at
the native port contract rather than inventing per-instrument negotiation.

The target validity/layout design is an ABI gate, not permission to start
bindings against guessed layouts. Byte counts, numeric type IDs and exact public
function names are assigned and tested in phase 1; semantic boundaries above
are settled. No unresolved optical/controller choice blocks this plan.

Use one coordinated breaking native/binding/dependency release. Do not mutate
published tags, retrofit existing sealed applications, or keep automatic old/new
metadata fallback. Development may progress in isolated source environments;
publish an installable cohort only after all applicable gates pass. Keep the
previous released installation available for rollback and stop only task-owned
sessions when switching. No existing session is migrated in place.

Capture schemas and GUI observation schemas change only where their actual
record representation requires it. Reject incompatible old captures without
rewriting historical bytes; an explicit offline converter is separate scope.
Remove acquisition-specific Properties helpers and keys from the new cohort;
inventory independent generic Properties users and migrate or explicitly exclude
them before claiming retirement. Do not preserve a second acquisition encoding.

### Verification and completion criteria

Every ACQ-META clause is planned, not yet implemented or qualified. During
delivery, extend the existing native ndarray traceability ledger with each ID,
owning change, selector and result; do not mark satisfied from document presence.

| Target clauses | Required verification |
| --- | --- |
| 001–003, 007 | Independent optional/required types, type order, custom IDs beyond bit 31, duplicate/undersized/oversized/misaligned records, bounded count/bytes, required type unavailable, unsupported type/layout and renegotiation |
| 004–006 | Alternating valid/invalid/absent states on recycled buffers, unknown optional bytes, insufficient output capacity, fanout immutability, retained/held/captured data, lease expiry, failure without partial publication/state advance |
| 008 | Full UInt64 identities, reset/generation boundaries, republishing with different Header sequence, F/U ordering, mixed exposure, missing trigger/optical records, declared versus absent mappings |
| TIME-001–005 | REALTIME and TAI values/event labels/range, monotonic deadlines through wall-clock steps, invalid mapping/TAI, same/different hosts/domains, unknown accuracy, model time, source-time preservation and explicitly labelled recorder receipt fallback |
| Entire contract | C/Julia/Rust round trips, stream/filter/ndarray/queue/row-block paths, capture/replay and GUI/native/browser, raw UInt16 GPI pixel-calibration path, independent-rate SCC/LLOWFS composition, CPU/captured CUDA and supported AMDGPU preparation |

Allocation and added copy/validation cost are measured separately from optics:
compare equivalent warmed metadata paths against the released cohort, including
maximum configured record count/size. Require bounded work, no locks/blocking
calls/per-frame metadata allocation, and no silent capacity reduction. Report
median/tail costs and regressions; no 50 Hz or hardware claim follows from unit
tests or cold exports. Physical clock accuracy requires separate device evidence.

Completion requires all supported producer/consumer profiles migrated or
explicitly excluded, verified public bindings, changed documentation/traceability,
registered coherent dependencies, fresh sealed applications and declared capture
compatibility. Software results, live application acceptance, clock accuracy and
deadline qualification remain separate claims. Retain concise receipts and usable
scientific inputs; remove temporary builds/logs after verification.

### Primary references

- [SPA buffer metadata array and shared skeleton](https://docs.pipewire.org/page_spa_buffer.html)
- [Standard Header fields](https://docs.pipewire.org/structspa__meta__header.html)
- [SPA metadata type/size/data](https://docs.pipewire.org/structspa__meta.html)
- [SPA buffer allocation](https://docs.pipewire.org/group__spa__buffer.html)
- [Linux REALTIME, TAI and monotonic clocks](https://man7.org/linux/man-pages/man3/clock_gettime.3.html)
- [RTP timestamps and RTCP clock correspondence](https://www.rfc-editor.org/rfc/rfc3550.html)
