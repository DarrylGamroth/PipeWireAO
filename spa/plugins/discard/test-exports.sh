#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
symbols=$("$1" -D --defined-only "$2")
test "$(printf '%s\n' "$symbols" | awk '{print $NF}')" = spa_handle_factory_enum
