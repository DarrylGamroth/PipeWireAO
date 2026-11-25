# Discard sink

Use the `api.pipewireao.discard` SPA factory to consume a stream and inspect
what reached the sink. The standard PipeWireAO configuration maps the factory
to `discard/libspa-pipewireao-discard`. Custom configurations can use the
same mapping. Public property names and IDs are in
[the discard header](../spa/include/spa/param/discard.h).

The sink has one input port. It accepts a fixed format, validates the incoming
buffer ID, records the buffer and returns `SPA_STATUS_NEED_DATA` to release it.
The process callback allocates no memory. It increments single-writer counters
and computes an ordered FNV-1a digest over the contiguous, CPU-addressable
part of each chunk. Processing cost therefore grows with the number of bytes
read. An unmapped block contributes to the advertised byte count and contributes
zero bytes to the digest. Read `discard.bytes` and `discard.digest-bytes` to
distinguish the two measures. The sink does not advertise custom acquisition
records through `SPA_PARAM_Meta`; a branch that requires those records needs a
metadata-capable endpoint.

The [factory test](../spa/plugins/discard/test-sink.c) exercises format and
buffer negotiation, pause, input errors, payload accounting and digest. The
[export check](../spa/plugins/discard/test-exports.sh) verifies the installed
library interface. The queue's cross-thread ownership and overflow policies
are documented in the [queue contract](queue.md).
