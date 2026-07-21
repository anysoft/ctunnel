#!/bin/sh
set -eu
bin=${1:-build/ctunnel}
keygen_bin=${CTUNNEL_KEYGEN_BIN:-$bin}
root=$(mktemp -d "${TMPDIR:-/tmp}/ctunnel-it.XXXXXX")
port_base=$((20000 + ($$ % 20000)))
server_port=$port_base
remote_encrypted_port=$((port_base + 1))
local_echo_port=$((port_base + 2))
remote_raw_port=$((port_base + 3))
proxy_port=$((port_base + 4))
server_pid=
client_pid=
echo_pid=
proxy_pid=
slow_pid=
# shellcheck disable=SC2329
cleanup() {
  [ -z "$client_pid" ] || wait_for_exit "$client_pid"
  [ -z "$server_pid" ] || wait_for_exit "$server_pid"
  [ -z "$echo_pid" ] || wait_for_exit "$echo_pid"
  [ -z "$proxy_pid" ] || wait_for_exit "$proxy_pid"
  [ -z "$slow_pid" ] || wait_for_exit "$slow_pid"
  rm -rf "$root"
}
trap cleanup EXIT INT TERM

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

"$keygen_bin" keygen --private "$root/server.key" --public "$root/server.pub" >/dev/null
"$keygen_bin" keygen --private "$root/client.key" --public "$root/client.pub" >/dev/null
cat >"$root/clients.ini" <<EOF
[client.integration]
public_key = $root/client.pub
allow_bind_addr = ::1
allow_remote_port = $remote_encrypted_port
allow_remote_port = $remote_raw_port
max_services = 2
max_streams = 8
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
server_port = $proxy_port
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
remote_port = $remote_encrypted_port
local_addr = 127.0.0.1
local_port = $local_echo_port
data_encryption = true
[echo-raw]
type = tcp
remote_addr = ::1
remote_port = $remote_raw_port
local_addr = 127.0.0.1
local_port = $local_echo_port
data_encryption = false
EOF
"$bin" -t -c "$root/server.ini" >/dev/null
"$bin" -t -c "$root/client.ini" >/dev/null
"$bin" -c "$root/server.ini" >"$root/server.log" 2>&1 & server_pid=$!
CTUNNEL_PROXY_PORT=$proxy_port CTUNNEL_SERVER_PORT=$server_port CTUNNEL_CAPTURE=$root/tunnel.capture \
python3 -c 'import os,socket,threading
listen_port=int(os.environ["CTUNNEL_PROXY_PORT"]);server_port=int(os.environ["CTUNNEL_SERVER_PORT"]);capture=os.environ["CTUNNEL_CAPTURE"]
lock=threading.Lock()
def pump(source,destination):
 try:
  while True:
   data=source.recv(65536)
   if not data: break
   with lock:
    with open(capture,"ab") as output: output.write(data)
   destination.sendall(data)
 finally:
  try: destination.shutdown(socket.SHUT_WR)
  except OSError: pass
def handle(client):
 try:
  upstream=socket.create_connection(("::1",server_port),2)
  upstream.settimeout(None)
 except OSError:
  client.close();return
 threading.Thread(target=pump,args=(client,upstream),daemon=True).start()
 threading.Thread(target=pump,args=(upstream,client),daemon=True).start()
listener=socket.socket(socket.AF_INET6);listener.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);listener.bind(("::1",listen_port));listener.listen()
while True: handle(listener.accept()[0])' & proxy_pid=$!
sleep .1
CTUNNEL_PROXY_PORT=$proxy_port python3 -c 'import os,socket,time
s=socket.create_connection(("::1",int(os.environ["CTUNNEL_PROXY_PORT"])),1)
s.sendall(b"C")
time.sleep(30)' & slow_pid=$!
CTUNNEL_ECHO_PORT=$local_echo_port python3 -c 'import os,socket
s=socket.socket();s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);s.bind(("127.0.0.1",int(os.environ["CTUNNEL_ECHO_PORT"])));s.listen()
while True:
 c,_=s.accept()
 first=True
 while True:
  b=c.recv(65536)
  if not b: break
  if first and b==b"ctunnel-large-response":
   c.sendall((b"0123456789abcdef"*32768))
   break
  first=False
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
  if CTUNNEL_REMOTE_ENCRYPTED_PORT=$remote_encrypted_port CTUNNEL_REMOTE_RAW_PORT=$remote_raw_port python3 -c 'import os,socket,concurrent.futures,signal
signal.alarm(5)
encrypted=int(os.environ["CTUNNEL_REMOTE_ENCRYPTED_PORT"])
raw=int(os.environ["CTUNNEL_REMOTE_RAW_PORT"])
def once(i):
 p=encrypted if i%2==0 else raw
 msg=("ctunnel-integration-%d"%i).encode()
 with socket.create_connection(("::1",p),.5) as s:
  s.settimeout(.5)
  s.sendall(msg);out=b""
  while len(out)<len(msg):
   chunk=s.recv(64)
   if not chunk: raise RuntimeError("connection closed before echo")
   out+=chunk
  assert out==msg
with concurrent.futures.ThreadPoolExecutor(max_workers=6) as x: list(x.map(once,range(6)))' 2>/dev/null; then
    CTUNNEL_REMOTE_ENCRYPTED_PORT=$remote_encrypted_port CTUNNEL_REMOTE_RAW_PORT=$remote_raw_port CTUNNEL_CAPTURE=$root/tunnel.capture python3 -c 'import os,socket,time
encrypted=b"CTUNNEL_SECRET_TEST_PATTERN_ENCRYPTED_7f6d"
raw=b"CTUNNEL_RAW_TEST_PATTERN_VISIBLE_2a91"
for port,message in ((int(os.environ["CTUNNEL_REMOTE_ENCRYPTED_PORT"]),encrypted),(int(os.environ["CTUNNEL_REMOTE_RAW_PORT"]),raw)):
 with socket.create_connection(("::1",port),1) as connection:
  connection.sendall(message)
  received=connection.recv(len(message))
  assert received==message
time.sleep(.1)
captured=open(os.environ["CTUNNEL_CAPTURE"],"rb").read()
assert encrypted not in captured
assert raw in captured
'
    CTUNNEL_REMOTE_ENCRYPTED_PORT=$remote_encrypted_port python3 -c 'import os,socket,signal
signal.alarm(8)
p=int(os.environ["CTUNNEL_REMOTE_ENCRYPTED_PORT"])
want=b"0123456789abcdef"*32768
with socket.create_connection(("::1",p),.5) as s:
 s.settimeout(1)
 s.sendall(b"ctunnel-large-response")
 s.shutdown(socket.SHUT_WR)
 out=bytearray()
 while len(out)<len(want):
  chunk=s.recv(65536)
  if not chunk: raise RuntimeError("connection closed before large response completed")
  out.extend(chunk)
 assert bytes(out)==want
' 2>/dev/null
    kill "$client_pid"
    wait_for_exit "$client_pid"
    client_pid=
    sleep .2
    "$bin" -c "$root/client.ini" >"$root/client-reconnect.log" 2>&1 & client_pid=$!
    j=0
    while [ "$j" -lt 40 ]; do
      if CTUNNEL_REMOTE_ENCRYPTED_PORT=$remote_encrypted_port CTUNNEL_REMOTE_RAW_PORT=$remote_raw_port python3 -c 'import os,socket,signal
signal.alarm(3)
encrypted=int(os.environ["CTUNNEL_REMOTE_ENCRYPTED_PORT"])
raw=int(os.environ["CTUNNEL_REMOTE_RAW_PORT"])
for p in (encrypted,raw):
 with socket.create_connection(("::1",p),.3) as s:
  s.settimeout(.5)
  s.sendall(b"reregistered")
  out=s.recv(32)
  if not out: raise RuntimeError("connection closed before reregister echo")
  assert out==b"reregistered"' 2>/dev/null; then
        echo "integration test passed (encrypted/raw, concurrent pool, reconnect/reregister)"
        exit 0
      fi
      j=$((j+1)); sleep .1
    done
    break
  fi
  i=$((i+1)); sleep .1
done
echo "ports: server=$server_port encrypted=$remote_encrypted_port raw=$remote_raw_port local_echo=$local_echo_port" >&2
ps -f -p "$server_pid" "$client_pid" "$echo_pid" >&2 || true
echo "server log:" >&2; sed -n '1,120p' "$root/server.log" >&2
echo "client log:" >&2; sed -n '1,120p' "$root/client.log" >&2
if [ -f "$root/client-reconnect.log" ]; then
  echo "reconnected client log:" >&2; sed -n '1,120p' "$root/client-reconnect.log" >&2
fi
exit 1
