#!/usr/bin/env bash

set -euo pipefail

cd "$(dirname "$0")/.."

port="${QEMU_GDB_PORT:-1234}"
ready_line='[QEMU] Waiting for GDB Connection'

port_is_open() {
    python3 - "$port" <<'PY'
import socket
import sys

port = int(sys.argv[1])
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.settimeout(0.2)
    try:
        sock.connect(("127.0.0.1", port))
    except OSError:
        raise SystemExit(1)
raise SystemExit(0)
PY
}

if port_is_open; then
    echo "[QEMU] GDB port ${port} is already in use. Stop the existing QEMU instance first." >&2
    exit 1
fi

watch_for_port() {
    for _ in $(seq 1 300); do
        if port_is_open; then
            echo "$ready_line"
            return 0
        fi

        sleep 0.1
    done

    echo "[QEMU] Timed out waiting for GDB port ${port}." >&2
    return 1
}

watch_for_port &
watcher_pid=$!

cleanup() {
    if kill -0 "$watcher_pid" 2>/dev/null; then
        kill "$watcher_pid" 2>/dev/null || true
        wait "$watcher_pid" 2>/dev/null || true
    fi
}

trap cleanup EXIT INT TERM

make qemu-gdb
