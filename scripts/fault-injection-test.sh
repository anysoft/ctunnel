#!/bin/sh
set -eu

bin=${CTUNNEL_BIN:-build/ctunnel}
case "${1:-fd-limit}" in
  fd-limit)
    if (ulimit -n 64) 2>/dev/null; then
      (
        ulimit -n 64
        scripts/stress-test.sh --bin "$bin" --connections 80 --concurrency 40 --bytes 512
      )
    else
      echo "fd-limit fault injection skipped: shell cannot lower RLIMIT_NOFILE" >&2
    fi
    ;;
  *)
    echo "usage: scripts/fault-injection-test.sh [fd-limit]" >&2
    exit 2
    ;;
esac
