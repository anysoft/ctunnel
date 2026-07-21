#!/bin/sh
set -eu

binary=${1:?usage: scripts/check-binary-config.sh BINARY ROLE [mini]}
role=${2:?usage: scripts/check-binary-config.sh BINARY ROLE [mini]}
profile=${3:-default}
symbols=$(mktemp "${TMPDIR:-/tmp}/ctunnel-symbols.XXXXXX")
texts=$(mktemp "${TMPDIR:-/tmp}/ctunnel-strings.XXXXXX")
patterns=$(mktemp "${TMPDIR:-/tmp}/ctunnel-patterns.XXXXXX")
trap 'rm -f "$symbols" "$texts" "$patterns"' EXIT INT TERM
nm_tool=${CTUNNEL_NM:-nm}
strings_tool=${CTUNNEL_STRINGS:-strings}
readelf_tool=${CTUNNEL_READELF:-readelf}
"$nm_tool" "$binary" >"$symbols" 2>/dev/null || true
"$strings_tool" "$binary" >"$texts"

case "$role" in
  server-only)
    if grep -q 'ct_run_client\|ct_applet_client' "$symbols"; then
      echo "client entry point found in server-only binary" >&2
      exit 1
    fi
    if grep -Fxq 'ctunnel-client' "$texts"; then
      echo "client alias found in server-only binary" >&2
      exit 1
    fi
    ;;
  client-only)
    if grep -q 'ct_run_server\|ct_applet_server' "$symbols"; then
      echo "server entry point found in client-only binary" >&2
      exit 1
    fi
    if grep -Fxq 'ctunnel-server' "$texts"; then
      echo "server alias found in client-only binary" >&2
      exit 1
    fi
    ;;
  both) ;;
  *) echo "unknown role: $role" >&2; exit 2 ;;
esac

if [ "$profile" = mini ]; then
  if grep -q 'ct_keygen\|ct_fingerprint\|ct_derive_data\|ct_applet_build_info\|ct_proxy_protocol_build' "$symbols"; then
    echo "disabled Mini symbol found" >&2
    grep 'ct_keygen\|ct_fingerprint\|ct_derive_data\|ct_applet_build_info\|ct_proxy_protocol_build' "$symbols" >&2 || true
    exit 1
  fi
  cat >"$patterns" <<'EOF'
ctunnel keygen
ctunnel fingerprint
ctunnel build-info
ctunnel build-config
public key fingerprint
ctunnel-v2/data/master
EOF
  if grep -F -f "$patterns" "$texts" >/dev/null || grep -Fx 'debug' "$texts" >/dev/null ||
     grep -Fx 'trace' "$texts" >/dev/null; then
    echo "disabled Mini command/log/encryption string found" >&2
    grep -F -f "$patterns" "$texts" >&2 || true
    grep -Fx 'debug' "$texts" >&2 || true
    grep -Fx 'trace' "$texts" >&2 || true
    exit 1
  fi
else
  if ! grep -q 'ct_proxy_protocol_build' "$symbols"; then
    echo "PROXY Protocol symbol missing from non-Mini binary" >&2
    exit 1
  fi
fi

if command -v "$readelf_tool" >/dev/null 2>&1; then
  if "$readelf_tool" -d "$binary" 2>/dev/null | grep -Eiq 'libsodium|libssl|libcrypto|mbedtls'; then
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
