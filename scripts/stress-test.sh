#!/bin/sh
set -eu

bin=${CTUNNEL_BIN:-build/ctunnel}
connections=100
concurrency=50
bytes=4096
rate=0
duration=
artifacts_dir=

while [ "$#" -gt 0 ]; do
  case "$1" in
    --bin) bin=$2; shift 2 ;;
    --connections) connections=$2; shift 2 ;;
    --concurrency) concurrency=$2; shift 2 ;;
    --bytes) bytes=$2; shift 2 ;;
    --rate) rate=$2; shift 2 ;;
    --duration) duration=$2; shift 2 ;;
    --artifacts-dir) artifacts_dir=$2; shift 2 ;;
    --help)
      echo "usage: scripts/stress-test.sh [--bin build/ctunnel] [--connections N] [--concurrency N] [--bytes N] [--rate N]"
      exit 0
      ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

root=$(mktemp -d "${TMPDIR:-/tmp}/ctunnel-stress.XXXXXX")
server_pid=
client_pid=
echo_pid=
server_port=
remote_port=
local_port=
cleanup() {
  if [ -n "$artifacts_dir" ]; then
    mkdir -p "$artifacts_dir"
    cp "$root"/*.log "$root"/*.stdout "$root"/*.stderr "$artifacts_dir"/ 2>/dev/null || true
    {
      echo "connections=$connections"
      echo "concurrency=$concurrency"
      echo "bytes=$bytes"
      echo "rate=$rate"
      echo "server_port=$server_port"
      echo "remote_port=$remote_port"
      echo "local_port=$local_port"
    } >"$artifacts_dir/parameters.txt"
  fi
  [ -z "$client_pid" ] || kill "$client_pid" 2>/dev/null || true
  [ -z "$server_pid" ] || kill "$server_pid" 2>/dev/null || true
  [ -z "$echo_pid" ] || kill "$echo_pid" 2>/dev/null || true
  [ -z "$client_pid" ] || wait "$client_pid" 2>/dev/null || true
  [ -z "$server_pid" ] || wait "$server_pid" 2>/dev/null || true
  [ -z "$echo_pid" ] || wait "$echo_pid" 2>/dev/null || true
  rm -rf "$root" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

port_base=$((26000 + ($$ % 10000)))
server_port=$port_base
remote_port=$((port_base + 1))
local_port=$((port_base + 2))

"$bin" keygen --private "$root/server.key" --public "$root/server.pub" >/dev/null
"$bin" keygen --private "$root/client.key" --public "$root/client.pub" >/dev/null
cat >"$root/clients.ini" <<EOF
[client.load]
public_key = $root/client.pub
allow_bind_addr = *
allow_remote_port = $remote_port
max_services = 4
max_streams = 256
EOF
cat >"$root/server.ini" <<EOF
[common]
mode = server
bind_addr = 127.0.0.1
bind_port = $server_port
identity_private_key = $root/server.key
authorized_clients_file = $root/clients.ini
max_clients = 2
max_services_per_client = 4
max_streams_per_client = 256
max_pending_streams = 256
log_level = info
log_file = $root/server.log
EOF
cat >"$root/client.ini" <<EOF
[common]
mode = client
server_addr = 127.0.0.1
server_port = $server_port
client_id = load
identity_private_key = $root/client.key
server_public_key = $root/server.pub
pool_count = 32
log_level = info
log_file = $root/client.log

[echo]
type = tcp
remote_addr = 127.0.0.1
remote_port = $remote_port
local_addr = 127.0.0.1
local_port = $local_port
data_encryption = true
EOF

python3 tests/load/echo_server.py --host 127.0.0.1 --port "$local_port" &
echo_pid=$!
if ! "$bin" -t -c "$root/server.ini" >"$root/server-configtest.log" 2>&1; then
  cat "$root/server-configtest.log" >&2
  exit 1
fi
if ! "$bin" -t -c "$root/client.ini" >"$root/client-configtest.log" 2>&1; then
  cat "$root/client-configtest.log" >&2
  exit 1
fi
"$bin" -c "$root/server.ini" >"$root/server.stdout" 2>"$root/server.stderr" &
server_pid=$!
"$bin" -c "$root/client.ini" >"$root/client.stdout" 2>"$root/client.stderr" &
client_pid=$!

i=0
while [ "$i" -lt 100 ]; do
  if python3 - "$remote_port" <<'PY' >/dev/null 2>&1
import socket,sys
with socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=.2) as s:
    s.sendall(b"ready")
    assert s.recv(5) == b"ready"
PY
  then
    break
  fi
  i=$((i + 1))
  sleep .1
done

set +e
python3 tools/ctunnel-load.py --host 127.0.0.1 --port "$remote_port" \
  --connections "$connections" --concurrency "$concurrency" --bytes "$bytes" --rate "$rate"
status=$?
set -e

echo "server log:" >&2
tail -n 80 "$root/server.log" >&2 || true
tail -n 80 "$root/server.stdout" >&2 || true
tail -n 80 "$root/server.stderr" >&2 || true
echo "client log:" >&2
tail -n 80 "$root/client.log" >&2 || true
tail -n 80 "$root/client.stdout" >&2 || true
tail -n 80 "$root/client.stderr" >&2 || true
exit "$status"
