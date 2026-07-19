#!/bin/sh
set -eu
bin=${1:-build/ctunnel}
keygen_bin=${CTUNNEL_KEYGEN_BIN:-$bin}
root=$(mktemp -d "${TMPDIR:-/tmp}/ctunnel-it.XXXXXX")
server_pid=
client_pid=
echo_pid=
cleanup() {
  [ -z "$client_pid" ] || kill "$client_pid" 2>/dev/null || true
  [ -z "$server_pid" ] || kill "$server_pid" 2>/dev/null || true
  [ -z "$echo_pid" ] || kill "$echo_pid" 2>/dev/null || true
  rm -rf "$root"
}
trap cleanup EXIT INT TERM
"$keygen_bin" keygen --private "$root/server.key" --public "$root/server.pub" >/dev/null
"$keygen_bin" keygen --private "$root/client.key" --public "$root/client.pub" >/dev/null
cat >"$root/clients.ini" <<EOF
[client.integration]
public_key = $root/client.pub
allow_bind_addr = ::1
allow_remote_port = 32222
allow_remote_port = 32224
max_services = 2
max_streams = 8
EOF
cat >"$root/server.ini" <<EOF
[common]
mode = server
bind_addr = ::1
bind_port = 37000
identity_private_key = $root/server.key
authorized_clients_file = $root/clients.ini
allowed_ciphers = xchacha20-poly1305
preferred_cipher = xchacha20-poly1305
heartbeat_interval = 2
heartbeat_timeout = 6
handshake_timeout = 3
max_clients = 2
max_services_per_client = 4
max_streams_per_client = 8
max_pending_streams = 4
log_level = info
EOF
cat >"$root/client.ini" <<EOF
[common]
mode = client
server_addr = ::1
server_port = 37000
client_id = integration
identity_private_key = $root/client.key
server_public_key = $root/server.pub
allowed_ciphers = xchacha20-poly1305
preferred_cipher = xchacha20-poly1305
heartbeat_interval = 2
heartbeat_timeout = 6
handshake_timeout = 3
connect_timeout = 3
reconnect_initial_delay = 1
reconnect_max_delay = 2
pool_count = 2
log_level = info
[echo]
type = tcp
remote_addr = ::1
remote_port = 32222
local_addr = 127.0.0.1
local_port = 32223
data_encryption = required
[echo-raw]
type = tcp
remote_addr = ::1
remote_port = 32224
local_addr = 127.0.0.1
local_port = 32223
data_encryption = disabled
EOF
"$bin" -t -c "$root/server.ini" >/dev/null
"$bin" -t -c "$root/client.ini" >/dev/null
"$bin" -c "$root/server.ini" >"$root/server.log" 2>&1 & server_pid=$!
python3 -c 'import socket
s=socket.socket();s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);s.bind(("127.0.0.1",32223));s.listen()
while True:
 c,_=s.accept()
 while True:
  b=c.recv(65536)
  if not b: break
  c.sendall(b)
 c.close()' & echo_pid=$!
"$bin" -c "$root/client.ini" >"$root/client.log" 2>&1 & client_pid=$!
if [ "${CTUNNEL_MEASURE:-0}" = 1 ]; then
  sleep .5
  echo "server_rss_kib=$(ps -o rss= -p "$server_pid" | tr -d ' ')"
  echo "client_rss_kib=$(ps -o rss= -p "$client_pid" | tr -d ' ')"
fi
i=0
while [ "$i" -lt 50 ]; do
  if python3 -c 'import socket,concurrent.futures
def once(i):
 p=32222 if i%2==0 else 32224
 msg=("ctunnel-integration-%d"%i).encode()
 s=socket.create_connection(("::1",p),.5);s.sendall(msg);out=b""
 while len(out)<len(msg): out+=s.recv(64)
 s.close();assert out==msg
with concurrent.futures.ThreadPoolExecutor(max_workers=6) as x: list(x.map(once,range(6)))' 2>/dev/null; then
    kill "$client_pid"
    wait "$client_pid" 2>/dev/null || true
    client_pid=
    sleep .2
    "$bin" -c "$root/client.ini" >"$root/client-reconnect.log" 2>&1 & client_pid=$!
    j=0
    while [ "$j" -lt 40 ]; do
      if python3 -c 'import socket
for p in (32222,32224):
 s=socket.create_connection(("::1",p),.3);s.sendall(b"reregistered");assert s.recv(32)==b"reregistered";s.close()' 2>/dev/null; then
        echo "integration test passed (encrypted/raw, concurrent pool, reconnect/reregister)"
        exit 0
      fi
      j=$((j+1)); sleep .1
    done
    break
  fi
  i=$((i+1)); sleep .1
done
echo "server log:" >&2; sed -n '1,120p' "$root/server.log" >&2
echo "client log:" >&2; sed -n '1,120p' "$root/client.log" >&2
if [ -f "$root/client-reconnect.log" ]; then
  echo "reconnected client log:" >&2; sed -n '1,120p' "$root/client-reconnect.log" >&2
fi
exit 1
