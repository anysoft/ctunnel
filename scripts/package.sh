#!/bin/sh
set -eu
version=${VERSION:-0.1.0-dev}
target=${TARGET:?set TARGET}
binary=${BINARY:-build/ctunnel}
stage=$(mktemp -d "${TMPDIR:-/tmp}/ctunnel-pkg.XXXXXX")
trap 'rm -rf "$stage"' EXIT INT TERM
mkdir -p "$stage/ctunnel-$version-$target/examples"
cp "$binary" README.md LICENSE "$stage/ctunnel-$version-$target/"
cp third_party/monocypher/LICENSE "$stage/ctunnel-$version-$target/MONOCYPHER-LICENSE"
cp examples/server.ini examples/client.ini examples/clients.ini "$stage/ctunnel-$version-$target/examples/"
config=${CONFIG:-.config}
if [ -f "$config" ]; then
  cp "$config" "$stage/ctunnel-$version-$target/ctunnel.config"
fi
link_report=${LINK_REPORT:-$binary.link-report.txt}
if [ -f "$link_report" ]; then
  cp "$link_report" "$stage/ctunnel-$version-$target/ctunnel.link-report.txt"
fi
tar -C "$stage" -czf "ctunnel-$version-$target.tar.gz" "ctunnel-$version-$target"
shasum -a 256 "ctunnel-$version-$target.tar.gz"
