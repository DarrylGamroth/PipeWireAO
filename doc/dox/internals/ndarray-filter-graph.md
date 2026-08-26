\page page_ndarray_filter_graph Ndarray filter-graph proof of concept

# Ndarray filter-graph proof of concept

Status: active proof-of-concept contract

Review date: 2026-08-25

## Scope

The proof of concept implements a synchronous graph of typed ndarray
operations. The graph host is C. Individual operations are ordinary shared
libraries and may be implemented in C, Rust, or another language that can
export the versioned C ABI in
`spa/filter-graph/ndarray-plugin.h`.

The proof of concept establishes the operation ABI, loader, graph validation,
intermediate buffer ownership, typed property routing, synchronous graph
execution, and a `pw_filter` adapter. The adapter exposes the composite as one
PipeWire node and translates external graph ports to ordinary SPA ndarray
Format, Buffers, and Meta parameters.

## Normative contract

This document and `spa/filter-graph/ndarray-plugin.h` are the normative
authorities for the FGN ABI and graph host. Uppercase requirement terms use the
meanings defined by BCP 14 (RFC 2119 and RFC 8174). There is no admitted
latency target yet; real-time requirements below constrain mechanism and
steady-state allocation rather than claiming a percentile latency.
Implementation and verification status is maintained in
`ndarray-filter-graph-traceability.toml` beside this document.

### FGN-ABI-001 — Versioned C boundary

An operation plugin MUST expose the exact ABI version requested by the host and
use only the declared C-layout structures, fixed-width scalar fields, pointers,
and callbacks across the shared-library boundary. Descriptors, port arrays,
formats, shapes, descriptor strings, property descriptors, choices, and their
strings remain valid until instance cleanup or library unload as documented in
the public header.

### FGN-LIFE-001 — Callback concurrency and ownership

The host and plugin MUST follow this callback matrix:

| Callback family | Calling context | May overlap `process()` | Borrowed data lifetime |
|---|---|---:|---|
| instantiate, cleanup | serial lifecycle owner | no | config only for instantiate call |
| port and property enumeration | serial admission owner | no | returned descriptor storage lives with instance |
| get property | serial control owner | yes | returned string through callback return and immediate POD copy |
| prepare properties | serial control owner | yes | assignments and strings only for prepare call |
| commit properties | data-loop owner at graph-cycle start | no node processing has begun | prepared object transfers to plugin |
| discard properties | serial control owner | yes | prepared object consumed by callback |
| prepare/commit/discard parameter | serial parameter owner | yes | input buffer only for prepare call |
| activate, deactivate, reset | serial lifecycle owner | no | no retained host data |
| property revision | control and data-loop observers | yes | none |
| process | data-loop owner | one data-loop caller | buffers only for process call |

A plugin copies any string, buffer content, or other borrowed input retained
after its callback returns. The host serializes control callbacks with one
another but does not lock the data loop; every control/data publication
therefore uses an explicit release/acquire or stronger protocol.

### FGN-BUF-001 — Buffer admission and completion

Before invoking a plugin, the host MUST admit exactly one data plane per
connected buffer, verify offset and extent arithmetic against `maxsize`, verify
element and known-metadata alignment, reject duplicate metadata types, and
reject overlap among data, chunk, metadata, input, and mutable output regions.
The admitted profile permits at most 16 metadata entries and 4096 total
metadata bytes per buffer. A plugin MUST preserve the supplied output offset,
leave output size zero until processing succeeds, publish the exact completed
size only after success, and leave size zero on failure.

Verification intent: exercise nonzero output offsets, shared descriptors with
shifted chunks, misaligned data, excessive and duplicate metadata, metadata
aliasing, and a plugin failure after output admission.

### FGN-SCHEMA-001 — Descriptor admission

The graph host MUST reject a plugin descriptor surface before activation when
IDs or names are duplicated, required names/descriptions/units are empty, a
descriptor, port, or property local name exceeds 255 UTF-8 bytes, flags are
unknown or contradictory, scalar kinds disagree, a range is unsupported or
invalid, a default violates its constraint, or choice names or values are duplicated.
The name limit keeps configured and namespaced lookup lossless. Boolean and
string descriptors do not use ordered ranges. One graph admits at most 1024
nodes, 4096 links, and 1024 external ports per direction. One node admits at
most 1024 ports and 4096 properties; one property admits at most 1024 choices,
and one transaction admits at most 4096 assignments per node.

### FGN-PROP-001 — Failure-atomic preparation

`spa_fgn_graph_set_props()` MUST parse the complete namespaced Props
structure, reject malformed/trailing/duplicate/unknown/non-runtime values, and
prepare every affected operation before publishing any of them. A preparation
failure discards every prepared object and leaves the active and pending graph
transaction unchanged.

### FGN-PROP-002 — Graph-cycle publication

One successfully prepared non-empty graph property transaction MUST occupy a
bounded pending slot and return `-EBUSY` while that slot or its unreclaimed
retired transaction is unavailable. At the beginning of one later graph cycle,
the data-loop owner publishes every affected operation before processing any
node, so one cycle cannot observe a transaction on only a subset of its
affected nodes. An empty transaction succeeds without occupying the slot.

### FGN-PROP-003 — Prepared-object lifetime

A plugin `prepare_props()` callback MUST return a self-contained prepared
object or an error and copy every string it retains. `commit_props()` consumes
that object without failure, allocation, blocking, destruction, or unwinding;
`discard_props()` consumes an unpublished object on the control path.

### FGN-PROP-004 — Property observation

The host MUST validate the type and descriptor constraint of every value
returned by `get_prop()` before building `SPA_PARAM_Props`. A plugin brackets
an externally visible change by advancing a monotonic 64-bit revision from an
even stable value to odd for the complete duration of every observable
mutation and then to a greater even value. Overlapping writers MUST keep the
revision odd until all writers complete. The graph rejects an
odd or changed control snapshot, reports one bounded process change flag, and
the PipeWire adapter retries and publishes the snapshot outside the data loop.

### FGN-RT-001 — Repeated graph path

`spa_fgn_graph_process()`, graph-transaction publication, property-revision
sampling, and operation `process()` callbacks MUST perform bounded work
without allocation, blocking, locks, system calls, reference-count destruction,
or unwinding. Graph construction, parsing, validation, preparation, and retired
object reclamation remain off the repeated path. This syscall-free boundary is
the FGN graph and operation path, not the outer PipeWire module notification:
that adapter MAY issue one coalesced event-source wake-up after construction,
but MUST NOT use a dynamically growing generic invoke queue from its data-loop
callback.

### FGN-PORT-001 — Ndarray role separation

The ABI MUST represent per-frame values as ordinary data ports, sparse large
prepared artifacts as parameter input ports, and low-rate scalars as
properties. A parameter update copies or prepares retained plugin state before
publication and returns `-EBUSY` rather than growing an unbounded queue.

### FGN-SPA-001 — Standard parameter projection

The PipeWire adapter MUST expose runtime scalar values through standard
`SPA_PARAM_PropInfo` and `SPA_PARAM_Props` while documenting that this view
does not preserve canonical units, local IDs, or distinct read-only,
construction-only, and graph-rebuild classes. The operation ABI remains the
complete descriptor authority.

### FGN-RUST-001 — Rust adaptation profile

A Rust operation MUST be `Send + Sync`, use an unsafe extension contract for
handwritten callback implementations, prevent unwinding across every C
callback, keep Rust-only layouts and allocator-owned handles behind an opaque
instance pointer, and use a panic-free, allocation-free processing
implementation. Adapter-owned `UnsafeCell` workspaces MUST document and
enforce the single-data-owner rule. The delivered build profile and tests must
establish ABI layout, callback containment, and warmed processing behavior for
each admitted plugin.

## Configuration

The graph uses the same relaxed JSON/config syntax as PipeWire's existing
filter chain:

```text
{
    nodes = [
        {
            type = ndarray
            name = calibration
            plugin = "/usr/lib/pipewire-ao/filter-graph/libcalculon-fgn.so"
            label = pixel-calibration
            config = {
                detector-width = 320
                row-block-rows = 16
            }
            props = {
                "flat-sequence" = 42
            }
        }
        {
            type = ndarray
            name = slopes
            plugin = "/usr/lib/pipewire-ao/filter-graph/libcalculon-fgn.so"
            label = shack-hartmann
            config = {
                subapertures = 64
            }
        }
    ]
    links = [
        { output = "calibration:out" input = "slopes:pixels" }
    ]
    inputs = [ "calibration:in" "slopes:reconstructor" ]
    outputs = [ "slopes:slopes" "slopes:flux" ]
}
```

`plugin` is passed to `dlopen()` and `label` selects a descriptor exported by
that library. Every port supplies one exact element type, shape, layout,
optional rate, schema, and profile after instance construction. Graph creation
rejects incompatible links and cycles before activation.

The graph can be loaded with standard PipeWire tools. For example:

```text
pw-cli load-module libpipewire-module-ndarray-filter-chain '{
    node.name = ao-composite
    filter.graph = {
        nodes = [
            {
                type = ndarray
                name = calibration
                plugin = "/usr/lib/pipewire-ao/filter-graph/libcalculon-fgn.so"
                label = pixel-calibration
                config = { detector-width = 320 }
            }
        ]
    }
}'
```

External graph inputs and outputs become ports named `node:port`. Parameter
inputs also set the standard `port.control = true` property.

## Processing and ownership

The host allocates fixed intermediate `spa_buffer` storage when it constructs
the graph. Each operation receives arrays of `spa_fgn_buffer`, which pair the
ordinary `spa_buffer` with its exact ndarray format. An operation may inspect
standard SPA data, chunk, Header, and Acquisition structures directly.

`process()` handles exactly one complete buffer quantum. It must not allocate,
block, retain or modify an input buffer, or unwind across the ABI. The host
executes operations in topological order on the caller's data-loop thread. No
scheduler boundary exists between operations in the composite. Input and
output structural, metadata, and data regions are distinct; an adapter rejects
overlapping regions before constructing language-level borrowed views. An
operation writes at the supplied output chunk offset, preserves it, keeps the
completed size zero until success, and explicitly propagates
any input metadata required by its output contract.

## Properties

Operation descriptors enumerate local scalar property information. The host
exports it as standard `SPA_PARAM_PropInfo` and `SPA_PARAM_Props`, qualifying
each name with its configured instance name:

```text
controller:loop-gain
controller:active-loop-gain
controller:requested-generation
controller:active-generation
```

ABI version 2 describes stable local IDs and names, Bool, Int, Long, Float,
Double, Id, and String values, defaults, descriptions, canonical units, ranges,
and enumerated choices. Every property has exactly one update class:

| Update class | Initial node configuration | Runtime `SPA_PARAM_Props` |
|---|---|---|
| read-only | no | no |
| runtime | yes | yes |
| construction-only | yes | no |
| graph-rebuild | yes | no; the adapter must rebuild the graph |

Ranges and choice sets are mutually exclusive. The graph validates descriptor
metadata and every incoming value before it calls a plugin. Enum choices become
standard SPA enum choices and label pairs. SPA PropInfo has no unit field, so
the canonical unit remains available at the operation ABI but is not preserved
as machine-readable metadata in the standard PipeWire parameter view.

A property update is a two-phase graph transaction. The host validates and
prepares every affected operation first. It commits none if any preparation
fails. A successful non-empty transaction occupies one graph-owned pending
slot. At the start of a later graph cycle, the data loop invokes every commit
callback before it processes any node. Each affected operation therefore sees
the update at its process boundary in the same graph cycle. Commit callbacks
use preallocated plugin storage and do not allocate or destroy prepared state.

The graph returns `-EBUSY` while the pending transaction or its control-path
reclamation slot is occupied. Individual plugins may apply a tighter bounded
capacity. Requested and active generations are monotonic and independently
observable; an empty graph transaction is a no-op and advances neither.

An operation may expose a monotonic property revision as an even/odd sequence
counter. The revision remains odd while one or more writers change observable
state and advances to a greater even value after the final writer completes.
The graph rejects a control snapshot
that sees an odd or changed revision, and samples the counter around
`process()`. It returns
`SPA_FGN_PROCESS_RESULT_PROPS_CHANGED` when externally visible values changed.
The PipeWire adapter schedules the property snapshot on the main loop; the data
loop never calls an external notification callback. Per-frame measurements and
validity still belong in ndarray outputs or metadata rather than properties.

Large matrices and prepared artifacts are ndarray buffers or plugin-owned
prepared state. They are not property values.

C plugins can declare descriptors as a static table. The initializer and
enumeration helpers in `spa/filter-graph/ndarray-plugin.h` keep SPA type tags,
structure sizes, and table iteration out of the callback body:

```c
static const struct spa_fgn_property_info properties[] = {
    SPA_FGN_PROPERTY_INFO_INIT(
        PROPERTY_GAIN,
        SPA_FGN_PROPERTY_FLAG_RUNTIME | SPA_FGN_PROPERTY_FLAG_RANGE,
        "gain", "Scalar gain", "1",
        SPA_FGN_VALUE_FLOAT_INIT(1.0f),
        SPA_FGN_VALUE_FLOAT_INIT(0.0f),
        SPA_FGN_VALUE_FLOAT_INIT(2.0f), 0, NULL),
};

return spa_fgn_enum_prop_info_table(properties,
        SPA_N_ELEMENTS(properties), index, info);
```

The plugin still owns its current-value mapping, replacement state, and
lock-free publication strategy. Those parts depend on the concrete C state
representation and cannot be inferred safely by the ABI.

## Large parameter ports

Reconstructors, calibration matrices, masks, reference vectors, and similarly
large state use sparse ndarray input ports marked
`SPA_FGN_PORT_FLAG_PARAMETER`. They are ordinary typed PipeWire ports at the
outer node boundary, but they do not supply a buffer on every frame cycle.
Their schema and profile distinguish their semantic role from frame data with
the same element type and shape.

The adapter hands an arrived parameter buffer to
`spa_fgn_graph_update_parameter()` on a serial control or worker context. The
operation's `prepare_parameter()` callback must copy the array, build plans, or
otherwise produce bounded plugin-owned state before returning. The borrowed
`spa_buffer` is recycled by the data loop after the worker finishes with it.
`commit_parameter()` publishes the prepared state, and `process()` adopts it at
the next frame boundary.
`spa_meta_header.seq` can carry the producer's update sequence; operations
expose requested and active sequences as read-only scalar properties.

The example operations preallocate two coefficient slots. The control thread
writes only the inactive slot and publishes its index with a release store.
The data loop observes that index with an acquire load, changes the active slot,
then releases the old slot. If another update arrives before adoption, prepare
returns `-EBUSY`; storage and work are therefore bounded. A production
reconstructor can use more preallocated slots, but it must specify its capacity
and full/coalescing policy rather than growing a queue.

The C and Rust publication rule is:

| Field | Writer | Reader | Publication rule |
|---|---|---|---|
| inactive parameter slot | serial control worker | data loop after publication | ordinary writes before release publication |
| requested slot | control worker | data loop | release store, acquire load |
| active slot | data loop | control worker | release store, acquire load |

The standalone fixtures publish requested and active parameter sequences in
separate atomics. One atomic publication word combines a two-bit active-writer
count with a 62-bit completed-generation counter. Writer completion increments
the generation and decrements the writer count in one indivisible transition,
so overlapping property, parameter, and process writers cannot transiently
expose an even revision. Callback ownership admits at most one control writer
and one process writer. Headerless updates derive their sequence from the
latest accepted requested sequence, not from a stale active slot.

No matrix copy, allocation, destruction, lock, or reference-count operation is
performed by `process()`.

## Rust operations

Rust operations use `crate-type = ["cdylib"]` and export
`spa_filter_graph_ndarray_plugin_get_interface`. Only `#[repr(C)]` structures,
fixed-width integers, pointers, and `extern "C"` callbacks cross the boundary.
The Rust side owns its plans and workspaces behind an opaque instance pointer.

Every exported Rust callback must prevent unwinding across the C boundary;
no Rust reference, slice, trait object, `String`, `Vec`, or allocator-owned
pointer is part of the ABI. Construction and property preparation may
allocate. `process()` and state adoption may not.

Calculon's raw `Operation` extension trait is unsafe and requires `Send + Sync`.
The declarative scientist-facing macros implement that boundary using audited
single-data-owner workspace adapters. Release artifacts use unwinding so the
callback barrier can translate a panic to `-EFAULT`; a release cdylib fixture
verifies this behavior through the exported C ABI.

Calculon's `calculon-fgn` crate is the maintained Rust adaptation profile. A
scientist-facing operation declaration supplies typed configuration, an
ordinary Calculon plan and workspace preparation function, scientific schema,
and property policy. Reusable operation-shape adapters own ports, formats,
checked buffer views, workspace access, error translation, and every C
callback. Property-enabled operations use the two-slot runtime; operations
without properties use a fixed-plan store without implementing a dummy
algorithm property interface. One registry exports multiple descriptors from
the same shared library. The adapter owns schema projection, plan publication,
generations, revision sequencing, panic containment, and retired-plan
reclamation. The `leaky-integrator-f32`, `pdm-command-power-limit-f32`,
`optical-gain-correction-f32`, `docrime-excitation-f32`, and
`docrime-binary-excitation-f32` plugins are loaded and executed by this
repository's C graph-host test; the leaky integrator also executes the shared
two-property Calculon fixture. The optical-gain and DO-CRIME declarations use
the generic bounded parameter-plan runtime: a sparse vector prepares a
complete replacement plan off loop and processing adopts it at the next frame
boundary without allocation or retired-plan destruction. The DO-CRIME pair
also demonstrates the reusable stateful-source declaration and lifecycle
reset.

The current Calculon profile uses `config` for construction-only and
graph-rebuild values. Its shared `PropertyRuntime` accepts only runtime
properties in live or initial `props` transactions. A future construction
adapter may translate initial properties before instantiation, but must not
pretend that a post-instantiation plan update is construction.

Calculon's `scripts/test_fgn_pipewire_live.py` starts an isolated PipeWireAO
core, loads the release plugin through this module, discovers the node and its
ports, checks their ndarray formats, enumerates standard PropInfo and Props,
submits a two-property update followed by a core synchronization, and performs
16 immediate module teardown iterations after synchronized property updates.
It does not yet provide the live ndarray
client needed to drive a process boundary and observe off-loop publication of
the newly active values.

The build contains C and Rust implementations of the same scalar-gain
and sparse-coefficient operation. The same test loads each shared library,
constructs two linked instances, applies namespaced SPA properties
transactionally, verifies bounded coefficient update back pressure and
frame-boundary adoption, processes a two-dimensional F32 ndarray, and verifies
standard Header propagation.

## Adapter boundaries and remaining work

`libpipewire-module-ndarray-filter-chain` negotiates exact ndarray formats and
standard Buffer, Header, and Acquisition parameters. It passes frame buffers
directly to `spa_fgn_graph_process()`, forwards graph PropInfo and Props, and
moves parameter preparation to a dedicated worker. One parameter buffer may be
in flight per parameter port. A newer arrival is rejected and recycled while
that slot is occupied; an operation's `-EBUSY` response is retried after the
next graph process boundary.

The adapter does not yet rebuild a graph when a format-defining configuration
value changes, expose the dropped-parameter counter as a node property, or
provide latency benchmark results. The operation ABI does not need to become a
full SPA-node ABI for those additions. Only the composite adapter participates
in the PipeWire graph.
