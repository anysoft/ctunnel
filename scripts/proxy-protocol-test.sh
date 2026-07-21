#!/bin/sh
set -eu

bin=${1:-build/ctunnel}
root=$(mktemp -d "${TMPDIR:-/tmp}/ctunnel-proxy.XXXXXX")
port_base=$((38000 + ($$ % 8000)))
server_port=$port_base
remote_v1_port=$((port_base + 1))
remote_v2_port=$((port_base + 2))
local_v1_port=$((port_base + 3))
local_v2_port=$((port_base + 4))
server_pid=
client_pid=
backend_v1_pid=
backend_v2_pid=

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
  [ -z "$backend_v1_pid" ] || wait_for_exit "$backend_v1_pid"
  [ -z "$backend_v2_pid" ] || wait_for_exit "$backend_v2_pid"
  rm -rf "$root"
}
trap cleanup EXIT INT TERM

"$bin" keygen --private "$root/server.key" --public "$root/server.pub" >/dev/null
"$bin" keygen --private "$root/client.key" --public "$root/client.pub" >/dev/null

cat >"$root/clients.ini" <<EOF
[client.proxy]
public_key = $root/client.pub
allow_bind_addr = ::1
allow_remote_port = $remote_v1_port
allow_remote_port = $remote_v2_port
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
client_id = proxy
identity_private_key = $root/client.key
server_public_key = $root/server.pub
allowed_ciphers = xchacha20-poly1305
preferred_cipher = xchacha20-poly1305
pool_count = 4
default_data_encryption = true
log_level = info

[v1]
type = tcp
remote_addr = ::1
remote_port = $remote_v1_port
local_addr = 127.0.0.1
local_port = $local_v1_port
data_encryption = true
proxy_protocol = v1

[v2]
type = tcp
remote_addr = ::1
remote_port = $remote_v2_port
local_addr = 127.0.0.1
local_port = $local_v2_port
data_encryption = true
proxy_protocol = v2
EOF

chmod 600 "$root"/*.ini "$root"/*.key

CTUNNEL_PROXY_PORT=$local_v1_port CTUNNEL_PROXY_MODE=v1 python3 - <<'PY' &
import os, socket
mode=os.environ["CTUNNEL_PROXY_MODE"]
port=int(os.environ["CTUNNEL_PROXY_PORT"])
s=socket.socket(socket.AF_INET); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
s.bind(("127.0.0.1", port)); s.listen()
while True:
    c,_=s.accept()
    if mode=="v1":
        header=b""
        while not header.endswith(b"\r\n"):
            header += c.recv(1)
        assert header.startswith(b"PROXY TCP")
    data=c.recv(1024)
    c.sendall(b"ok:"+data)
    c.close()
PY
backend_v1_pid=$!

CTUNNEL_PROXY_PORT=$local_v2_port CTUNNEL_PROXY_MODE=v2 python3 - <<'PY' &
import os, socket, struct
port=int(os.environ["CTUNNEL_PROXY_PORT"])
sig=b"\r\n\r\n\x00\r\nQUIT\n"
s=socket.socket(socket.AF_INET); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
s.bind(("127.0.0.1", port)); s.listen()
while True:
    c,_=s.accept()
    header=c.recv(16)
    assert header[:12] == sig
    assert header[12] == 0x21
    assert header[13] in (0x11, 0x21)
    n=struct.unpack("!H", header[14:16])[0]
    body=b""
    while len(body)<n:
        body += c.recv(n-len(body))
    data=c.recv(1024)
    c.sendall(b"ok:"+data)
    c.close()
PY
backend_v2_pid=$!

"$bin" -c "$root/server.ini" >"$root/server.log" 2>&1 &
server_pid=$!
sleep .2
"$bin" -c "$root/client.ini" >"$root/client.log" 2>&1 &
client_pid=$!

for port in "$remote_v1_port" "$remote_v2_port"; do
  i=0
  while [ "$i" -lt 50 ]; do
    if CTUNNEL_PROXY_REMOTE_PORT=$port python3 - <<'PY' 2>/dev/null; then
import os, socket
p=int(os.environ["CTUNNEL_PROXY_REMOTE_PORT"])
with socket.create_connection(("::1", p), .5) as s:
    s.settimeout(1)
    s.sendall(b"hello")
    assert s.recv(32) == b"ok:hello"
PY
      break
    fi
    i=$((i + 1))
    sleep .1
  done
  if [ "$i" -ge 50 ]; then
    echo "PROXY Protocol smoke failed for port $port" >&2
    echo "server log:" >&2; sed -n '1,160p' "$root/server.log" >&2
    echo "client log:" >&2; sed -n '1,160p' "$root/client.log" >&2
    exit 1
  fi
done

echo "proxy protocol smoke passed"
