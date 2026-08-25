\page page_ndarray_filter_graph Ndarray filter-graph proof of concept

# Ndarray filter-graph proof of concept

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
scheduler boundary exists between operations in the composite.

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

A property update is a two-phase graph transaction. The host validates and
prepares every affected operation first. It commits none if any preparation
fails. A plugin's commit callback publishes requested state; the operation
adopts that state at its next `process()` boundary and reports active state
through read-only properties. Property updates run serially on the control
loop. Publication to `process()` must still use a lock-free synchronization
scheme; the example plugins publish a packed gain and generation atomically.

Large matrices and prepared artifacts are ndarray buffers or plugin-owned
prepared state. They are not property values.

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

No matrix copy, allocation, destruction, lock, or reference-count operation is
performed by `process()`.

## Rust operations

Rust operations use `crate-type = ["cdylib"]` and export
`spa_filter_graph_ndarray_plugin_get_interface`. Only `#[repr(C)]` structures,
fixed-width integers, pointers, and `extern "C"` callbacks cross the boundary.
The Rust side owns its plans and workspaces behind an opaque instance pointer.

Every exported Rust callback must contain panics and convert them to an error;
no Rust reference, slice, trait object, `String`, `Vec`, or allocator-owned
pointer is part of the ABI. Construction and property preparation may
allocate. `process()` and state adoption may not.

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
