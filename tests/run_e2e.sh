#!/usr/bin/env bash
# e2e runner backed by the Go test servers.
#   tests/testserver  (stdlib-only): HTTP echo + GraphQL + WebSocket on one ephemeral port.
#   tests/grpcserver  (needs grpc-go in the module cache): reflection echo gRPC on its own port.
# Builds + starts whichever servers the requested pairs need, runs each e2e binary against the right one,
# then tears the servers down. Propagates the first non-zero exit code.
# Args: one or more "<binary>:<scheme>" pairs (scheme = http | ws | grpc), e.g.
#   run_e2e.sh /path/http_e2e:http /path/ws_e2e:ws /path/grpc_e2e:grpc
# SKIPs (exit 0) the whole run if the Go toolchain is missing; SKIPs only the grpc pair if grpcserver can't
# build offline (grpc-go not cached).
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GO="$(command -v go || true)"
if [ -z "$GO" ]; then echo "SKIP: go toolchain not found"; exit 0; fi
if [ "$#" -lt 1 ]; then echo "usage: run_e2e.sh <bin:scheme> [<bin:scheme>...]"; exit 2; fi

BIN_DIR="$(mktemp -d -t deed_testserver.XXXXXX)"
HTTP_PID=""; GRPC_PID=""
cleanup() {
  [ -n "$HTTP_PID" ] && { kill "$HTTP_PID" 2>/dev/null; wait "$HTTP_PID" 2>/dev/null; }
  [ -n "$GRPC_PID" ] && { kill "$GRPC_PID" 2>/dev/null; wait "$GRPC_PID" 2>/dev/null; }
  rm -rf "$BIN_DIR"
}
trap cleanup EXIT

# Wait for "LISTENING <port>" on a server's stdout log; echoes the port (empty on timeout).
wait_port() {
  local log="$1" port=""
  for _ in $(seq 1 50); do
    local line; line="$(head -n1 "$log" 2>/dev/null || true)"
    case "$line" in LISTENING\ *) port="${line#LISTENING }"; break ;; esac
    sleep 0.1
  done
  echo "$port"
}

# Which servers do the requested pairs need?
WANT_HTTP=0; WANT_GRPC=0
for pair in "$@"; do
  case "${pair##*:}" in grpc) WANT_GRPC=1 ;; *) WANT_HTTP=1 ;; esac
done

HTTP_PORT=""; GRPC_PORT=""
if [ "$WANT_HTTP" = 1 ]; then
  HTTP_SRV="$BIN_DIR/testserver"
  ( cd "$HERE/testserver" && "$GO" build -o "$HTTP_SRV" . ) || { echo "FAIL: go build testserver"; exit 1; }
  "$HTTP_SRV" 0 >"$BIN_DIR/http.log" 2>&1 & HTTP_PID=$!
  HTTP_PORT="$(wait_port "$BIN_DIR/http.log")"
  if [ -z "$HTTP_PORT" ]; then echo "FAIL: http test server did not start"; cat "$BIN_DIR/http.log"; exit 1; fi
  echo "http/ws test server on 127.0.0.1:$HTTP_PORT (pid $HTTP_PID)"
fi

GRPC_OK=1
if [ "$WANT_GRPC" = 1 ]; then
  GRPC_SRV="$BIN_DIR/grpcserver"
  if ( cd "$HERE/grpcserver" && "$GO" build -o "$GRPC_SRV" . ) 2>"$BIN_DIR/grpcbuild.log"; then
    "$GRPC_SRV" 0 >"$BIN_DIR/grpc.log" 2>&1 & GRPC_PID=$!
    GRPC_PORT="$(wait_port "$BIN_DIR/grpc.log")"
    if [ -z "$GRPC_PORT" ]; then echo "FAIL: grpc test server did not start"; cat "$BIN_DIR/grpc.log"; exit 1; fi
    echo "grpc test server on 127.0.0.1:$GRPC_PORT (pid $GRPC_PID)"
  else
    GRPC_OK=0
    echo "SKIP grpc_e2e: grpcserver did not build (grpc-go not cached / offline)"
  fi
fi

rc=0
for pair in "$@"; do
  bin="${pair%%:*}"; scheme="${pair##*:}"
  if [ "$scheme" = grpc ]; then
    [ "$GRPC_OK" = 1 ] || { echo "--- skipped $(basename "$bin") (grpc) ---"; continue; }
    echo "--- running $(basename "$bin") (grpc) ---"
    "$bin" "grpc://127.0.0.1:$GRPC_PORT" || rc=$?
  else
    echo "--- running $(basename "$bin") ($scheme) ---"
    "$bin" "$scheme://127.0.0.1:$HTTP_PORT" || rc=$?
  fi
done
exit "$rc"
