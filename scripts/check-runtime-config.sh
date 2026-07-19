#!/bin/sh
set -eu

binary=${1:?usage: scripts/check-runtime-config.sh BINARY ROLE}
role=${2:?usage: scripts/check-runtime-config.sh BINARY ROLE}
temporary=$(mktemp -d "${TMPDIR:-/tmp}/ctunnel-runtime-config.XXXXXX")
trap 'rm -rf "$temporary"' EXIT INT TERM

expect_failure() {
  name=$1
  pattern=$2
  if "$binary" "$role" -c "$temporary/$name.ini" >"$temporary/$name.out" 2>&1; then
    echo "$name configuration unexpectedly succeeded" >&2
    exit 1
  fi
  if ! grep -Eiq "$pattern" "$temporary/$name.out"; then
    echo "$name did not report the expected reason" >&2
    cat "$temporary/$name.out" >&2
    exit 1
  fi
}

if [ "$role" = server ]; then
  cat >"$temporary/wrong-role.ini" <<EOF
[common]
mode = client
identity_private_key = missing
server_addr = ::1
server_public_key = missing
client_id = test
EOF
  expect_failure wrong-role 'client|role|unavailable'

  cat >"$temporary/ipv4.ini" <<EOF
[common]
mode = server
identity_private_key = missing
authorized_clients_file = missing
bind_addr = 127.0.0.1
max_clients = 1
EOF
  expect_failure ipv4 'IPv4|compiled'

  cat >"$temporary/encryption.ini" <<EOF
[common]
mode = server
identity_private_key = missing
authorized_clients_file = missing
bind_addr = ::1
max_clients = 1
default_data_encryption = required
EOF
  expect_failure encryption 'encryption|unavailable|compiled'

  cat >"$temporary/limit.ini" <<EOF
[common]
mode = server
identity_private_key = missing
authorized_clients_file = missing
bind_addr = ::1
max_clients = 999
EOF
  expect_failure limit 'max_clients|limit|invalid'
elif [ "$role" = client ]; then
  cat >"$temporary/wrong-role.ini" <<EOF
[common]
mode = server
identity_private_key = missing
authorized_clients_file = missing
EOF
  expect_failure wrong-role 'server|role|unavailable'
else
  echo "role must be server or client" >&2
  exit 2
fi

echo "runtime compiled-capability checks: OK ($role)"
