#!/bin/sh
set -eu

binary=${1:?usage: scripts/size-report.sh BINARY}
if [ ! -f "$binary" ]; then
  echo "size-report: binary not found: $binary" >&2
  exit 2
fi

file_tool=${CTUNNEL_FILE:-file}
strip_tool=${CTUNNEL_STRIP:-strip}
size_tool=${CTUNNEL_SIZE:-size}
nm_tool=${CTUNNEL_NM:-nm}
readelf_tool=${CTUNNEL_READELF:-readelf}
objdump_tool=${CTUNNEL_OBJDUMP:-objdump}

bytes() {
  wc -c <"$1" | tr -d ' '
}

echo "binary: $binary"
echo "file_bytes_unstripped: $(bytes "$binary")"
if command -v "$file_tool" >/dev/null 2>&1; then
  "$file_tool" "$binary"
else
  echo "file: unavailable (tool '$file_tool' not found)"
fi

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
if [ "$(uname -s)" = Darwin ] && command -v "$size_tool" >/dev/null 2>&1; then
  "$size_tool" -m "$binary" || true
elif command -v "$size_tool" >/dev/null 2>&1; then
  "$size_tool" -A "$binary" || "$size_tool" "$binary" || true
else
  echo "size: unavailable (tool '$size_tool' not found)"
fi
if command -v "$readelf_tool" >/dev/null 2>&1; then
  "$readelf_tool" -W -S "$binary" || true
fi

echo "dynamic_dependencies:"
if command -v "$readelf_tool" >/dev/null 2>&1; then
  "$readelf_tool" -W -d "$binary" | grep -E 'NEEDED|RPATH|RUNPATH|interpreter' || echo "none recorded"
elif command -v otool >/dev/null 2>&1; then
  otool -L "$binary"
elif command -v "$objdump_tool" >/dev/null 2>&1; then
  "$objdump_tool" -p "$binary" | grep -E 'DLL Name|NEEDED|RPATH|RUNPATH' || echo "none recorded"
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
if ! command -v "$nm_tool" >/dev/null 2>&1; then
  echo "nm: unavailable (tool '$nm_tool' not found)"
elif "$nm_tool" --help 2>&1 | grep -q -- '--size-sort'; then
  "$nm_tool" -S --size-sort --print-size "$binary" 2>/dev/null | tail -20 || true
else
  "$nm_tool" -nm "$binary" 2>/dev/null | tail -20 || true
fi

echo "monocypher_symbols:"
if command -v "$nm_tool" >/dev/null 2>&1; then
  "$nm_tool" "$binary" 2>/dev/null | grep -E '(_| )crypto_(aead|blake2b|eddsa|x25519|verify|wipe)' || true
else
  echo "nm: unavailable (tool '$nm_tool' not found)"
fi

limit_kib=${CTUNNEL_MAX_MINI_BINARY_SIZE_KIB:-0}
if [ "$limit_kib" -gt 0 ]; then
  limit_bytes=$((limit_kib * 1024))
  echo "stripped_limit_bytes: $limit_bytes"
  if [ "$stripped_bytes" -gt "$limit_bytes" ]; then
    echo "size-report: stripped binary exceeds ${limit_kib} KiB" >&2
    exit 1
  fi
fi
