#!/bin/sh
set -eu

binary=${1:?usage: scripts/size-report.sh BINARY}
if [ ! -f "$binary" ]; then
  echo "size-report: binary not found: $binary" >&2
  exit 2
fi

bytes() {
  wc -c <"$1" | tr -d ' '
}

echo "binary: $binary"
echo "file_bytes_unstripped: $(bytes "$binary")"
file "$binary"

link_report=${CTUNNEL_LINK_REPORT:-$binary.link-report.txt}
report_value() {
  key=$1
  if [ -f "$link_report" ]; then
    sed -n "s/^${key}: //p" "$link_report" | head -n 1
  fi
}
link_mode=${CTUNNEL_LINK_MODE:-$(report_value link_mode)}
libc_name=${CTUNNEL_LIBC:-$(report_value libc)}
compiler_name=${CTUNNEL_COMPILER:-$(report_value compiler)}
target_triplet=${CTUNNEL_TARGET_TRIPLET:-$(report_value target_triplet)}
echo "link_mode: ${link_mode:-unknown}"
echo "libc: ${libc_name:-unknown}"
echo "compiler: ${compiler_name:-unknown}"
echo "target_triplet: ${target_triplet:-unknown}"
if [ -f "$link_report" ]; then
  echo "link_verification_report: $link_report"
fi

temporary=$(mktemp "${TMPDIR:-/tmp}/ctunnel-size.XXXXXX")
cleanup() { rm -f "$temporary"; }
trap cleanup EXIT INT TERM
cp "$binary" "$temporary"
strip_tool=${CTUNNEL_STRIP:-strip}
if command -v "$strip_tool" >/dev/null 2>&1; then
  if [ "$(uname -s)" = Darwin ]; then
    "$strip_tool" -x "$temporary" 2>/dev/null || "$strip_tool" "$temporary"
  else
    "$strip_tool" --strip-unneeded "$temporary" 2>/dev/null || "$strip_tool" "$temporary"
  fi
  stripped_bytes=$(bytes "$temporary")
  echo "file_bytes_stripped: $stripped_bytes"
else
  stripped_bytes=$(bytes "$binary")
  echo "file_bytes_stripped: unavailable (strip tool '$strip_tool' not found)"
fi

echo "section_sizes:"
if [ "$(uname -s)" = Darwin ] && command -v size >/dev/null 2>&1; then
  size -m "$binary" || true
elif command -v size >/dev/null 2>&1; then
  size -A "$binary" || size "$binary" || true
fi
if command -v readelf >/dev/null 2>&1; then
  readelf -W -S "$binary" || true
fi

echo "dynamic_dependencies:"
if command -v readelf >/dev/null 2>&1; then
  readelf -W -d "$binary" | grep -E 'NEEDED|RPATH|RUNPATH|interpreter' || echo "none recorded"
elif command -v otool >/dev/null 2>&1; then
  otool -L "$binary"
else
  echo "dependency inspection tool unavailable"
fi

echo "link_verification:"
if [ -f "$link_report" ]; then
  sed -n '/^readelf program headers/,/^readelf dynamic section/p' "$link_report" || true
  sed -n '/^readelf dynamic section/,/^ldd/p' "$link_report" || true
  sed -n '/^otool dependencies/,$p' "$link_report" || true
  sed -n '/^dumpbin dependents/,$p' "$link_report" || true
  sed -n '/^objdump PE headers/,$p' "$link_report" || true
else
  echo "post-link report unavailable"
fi

echo "largest_symbols:"
if nm --help 2>&1 | grep -q -- '--size-sort'; then
  nm -S --size-sort --print-size "$binary" 2>/dev/null | tail -20 || true
else
  nm -nm "$binary" 2>/dev/null | tail -20 || true
fi

echo "monocypher_symbols:"
nm "$binary" 2>/dev/null | grep -E '(_| )crypto_(aead|blake2b|eddsa|x25519|verify|wipe)' || true

limit_kib=${CTUNNEL_MAX_MINI_BINARY_SIZE_KIB:-0}
if [ "$limit_kib" -gt 0 ]; then
  limit_bytes=$((limit_kib * 1024))
  echo "stripped_limit_bytes: $limit_bytes"
  if [ "$stripped_bytes" -gt "$limit_bytes" ]; then
    echo "size-report: stripped binary exceeds ${limit_kib} KiB" >&2
    exit 1
  fi
fi
