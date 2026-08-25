\page page_ndarray_filter_graph Ndarray filter-graph proof of concept

# Ndarray filter-graph proof of concept

## Scope

The proof of concept implements a synchronous graph of typed ndarray
operations. The graph host is C. Individual operations are ordinary shared
libraries and may be implemented in C, Rust, or another language that can
export the versioned C ABI in
`spa/filter-graph/ndarray-plugin.h`.

The proof of concept establishes the operation ABI, loader, graph validation,
intermediate buffer ownership, typed property routing, and synchronous graph
execution. It does not yet expose the graph as a PipeWire node. A node adapter
still needs to translate its external ports to ordinary SPA ndarray Format,
Buffers, Meta, and IO parameters.

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
    inputs = [ "calibration:in" ]
    outputs = [ "slopes:slopes" "slopes:flux" ]
}
```

`plugin` is passed to `dlopen()` and `label` selects a descriptor exported by
that library. Every port supplies one exact element type, shape, layout,
optional rate, schema, and profile after instance construction. Graph creation
rejects incompatible links and cycles before activation.

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
operation. The same test loads each shared library, constructs two linked
instances, applies namespaced SPA properties transactionally, processes a
two-dimensional F32 ndarray, and verifies standard Header propagation.

## Remaining node-adapter work

The next layer is a SPA or `pw_filter` adapter that exposes the composite as
one PipeWire node. It must:

- enumerate external formats with `spa_format_ndarray_build()`;
- negotiate standard Buffer, Meta, and IO parameters for every external port;
- pass dequeued `spa_buffer` objects to `spa_fgn_graph_process()`;
- forward graph PropInfo and Props parameters;
- publish active/status changes from the main loop; and
- rebuild the graph when a format-defining configuration value changes.

The operation ABI does not need to become a full SPA-node ABI for that layer.
Only the composite adapter participates in the PipeWire graph.
