/* PipeWireAO */
/* SPDX-License-Identifier: MIT */

/*
 * Keep the header-only SPA fast path for native callers while also emitting
 * callable symbols for FFI consumers of libpipewire-ao.
 */
#define SPA_API_META SPA_EXPORT
#include <spa/buffer/meta.h>
