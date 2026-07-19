#!/bin/sh
set -eu

binary=${1:?usage: scripts/check-binary-config.sh BINARY ROLE [mini]}
role=${2:?usage: scripts/check-binary-config.sh BINARY ROLE [mini]}
profile=${3:-default}
symbols=$(mktemp "${TMPDIR:-/tmp}/ctunnel-symbols.XXXXXX")
texts=$(mktemp "${TMPDIR:-/tmp}/ctunnel-strings.XXXXXX")
trap 'rm -f "$symbols" "$texts"' EXIT INT TERM
nm "$binary" >"$symbols" 2>/dev/null || true
strings "$binary" >"$texts"

case "$role" in
  server-only)
    if grep -q 'ct_run_client\|ct_applet_client' "$symbols"; then
      echo "client entry point found in server-only binary" >&2
      exit 1
    fi
    if grep -q 'ctunnel-client' "$texts"; then
      echo "client alias found in server-only binary" >&2
      exit 1
    fi
    ;;
  client-only)
    if grep -q 'ct_run_server\|ct_applet_server' "$symbols"; then
      echo "server entry point found in client-only binary" >&2
      exit 1
    fi
    if grep -q 'ctunnel-server' "$texts"; then
      echo "server alias found in client-only binary" >&2
      exit 1
    fi
    ;;
  both) ;;
  *) echo "unknown role: $role" >&2; exit 2 ;;
esac

if [ "$profile" = mini ]; then
  if grep -q 'ct_keygen\|ct_fingerprint\|ct_derive_data\|ct_applet_build_info' "$symbols"; then
    echo "disabled Mini symbol found" >&2
    exit 1
  fi
  if grep -Eq 'keygen|fingerprint|build-config|build-info|debug|trace|data/master' "$texts"; then
    echo "disabled Mini command/log/encryption string found" >&2
    exit 1
  fi
fi

if command -v readelf >/dev/null 2>&1; then
  if readelf -d "$binary" 2>/dev/null | grep -Eiq 'libsodium|libssl|libcrypto|mbedtls'; then
    echo "unexpected external cryptographic library dependency" >&2
    exit 1
  fi
elif command -v otool >/dev/null 2>&1; then
  if otool -L "$binary" | grep -Eiq 'libsodium|libssl|libcrypto|mbedtls'; then
    echo "unexpected external cryptographic library dependency" >&2
    exit 1
  fi
fi

echo "binary configuration check: OK ($role, $profile)"
