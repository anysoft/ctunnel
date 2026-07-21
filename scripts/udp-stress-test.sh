#!/bin/sh
set -eu

bin=${1:-build/ctunnel}
root=$(mktemp -d "${TMPDIR:-/tmp}/ctunnel-udp.XXXXXX")
port_base=$((30000 + ($$ % 10000)))
server_port=$port_base
remote_udp_port=$((port_base + 1))
local_udp_port=$((port_base + 2))
server_pid=
client_pid=
echo_pid=

wait_for_exit() {
  pid=$1
  kill "$pid" 2>/dev/null || true
  n=0
  while kill -0 "$pid" 2>/dev/null && [ "$n" -lt 50 ]; do
    n=$((n + 1))
    sleep .1
  done
  if kill -0 "$pid" 2>/dev/null; then
    kill -KILL "$pid" 2>/dev/null || true
  fi
  wait "$pid" 2>/dev/null || true
}

cleanup() {
  [ -z "$client_pid" ] || wait_for_exit "$client_pid"
  [ -z "$server_pid" ] || wait_for_exit "$server_pid"
  [ -z "$echo_pid" ] || wait_for_exit "$echo_pid"
  rm -rf "$root"
}
trap cleanup EXIT INT TERM

"$bin" keygen --private "$root/server.key" --public "$root/server.pub" >/dev/null
"$bin" keygen --private "$root/client.key" --public "$root/client.pub" >/dev/null

cat >"$root/clients.ini" <<EOF
[client.udp]
public_key = $root/client.pub
allow_bind_addr = ::1
allow_remote_port = $remote_udp_port
max_services = 4
max_streams = 64
EOF

cat >"$root/server.ini" <<EOF
[common]
mode = server
bind_addr = ::1
bind_port = $server_port
identity_private_key = $root/server.key
authorized_clients_file = $root/clients.ini
allowed_ciphers = xchacha20-poly1305
preferred_cipher = xchacha20-poly1305
log_level = info
EOF

cat >"$root/client.ini" <<EOF
[common]
mode = client
server_addr = ::1
server_port = $server_port
client_id = udp
identity_private_key = $root/client.key
server_public_key = $root/server.pub
allowed_ciphers = xchacha20-poly1305
preferred_cipher = xchacha20-poly1305
pool_count = 0
default_data_encryption = true
log_level = info

[echo]
type = udp
remote_addr = ::1
remote_port = $remote_udp_port
local_addr = 127.0.0.1
local_port = $local_udp_port
data_encryption = true
udp_idle_timeout = 10
udp_max_sessions = 128
udp_max_datagram_size = 1472
EOF

chmod 600 "$root"/*.ini "$root"/*.key

CTUNNEL_UDP_LOCAL_PORT=$local_udp_port python3 - <<'PY' &
import os, socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(os.environ["CTUNNEL_UDP_LOCAL_PORT"])))
while True:
    data, addr = s.recvfrom(65535)
    s.sendto(b"echo:" + data, addr)
PY
echo_pid=$!

"$bin" -c "$root/server.ini" >"$root/server.log" 2>&1 &
server_pid=$!
sleep .2
"$bin" -c "$root/client.ini" >"$root/client.log" 2>&1 &
client_pid=$!

i=0
while [ "$i" -lt 50 ]; do
  if python3 tools/udp-load.py --host ::1 --port "$remote_udp_port" --count "${CTUNNEL_UDP_COUNT:-100}" --size "${CTUNNEL_UDP_SIZE:-64}" 2>/dev/null; then
    exit 0
  fi
  i=$((i + 1))
  sleep .1
done

echo "UDP stress failed: server=$server_port remote_udp=$remote_udp_port local_udp=$local_udp_port" >&2
echo "server log:" >&2
sed -n '1,160p' "$root/server.log" >&2
echo "client log:" >&2
sed -n '1,160p' "$root/client.log" >&2
exit 1
