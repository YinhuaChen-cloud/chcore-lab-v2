#!/usr/bin/env bash

set -euo pipefail

pids="$(pgrep -f 'qemu-system-aarch64 .*tcp::1234' || true)"

if [[ -z "$pids" ]]; then
    exit 0
fi

kill $pids
