#!/bin/sh
set -eu

duration=24h
connections=200
concurrency=100
bytes=4096

while [ "$#" -gt 0 ]; do
  case "$1" in
    --duration) duration=$2; shift 2 ;;
    --connections) connections=$2; shift 2 ;;
    --concurrency) concurrency=$2; shift 2 ;;
    --bytes) bytes=$2; shift 2 ;;
    --help)
      echo "usage: scripts/soak-test.sh [--duration 24h] [--connections N] [--concurrency N] [--bytes N]"
      exit 0
      ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

case "$duration" in
  *h) seconds=$(( ${duration%h} * 3600 )) ;;
  *m) seconds=$(( ${duration%m} * 60 )) ;;
  *s) seconds=${duration%s} ;;
  *) seconds=$duration ;;
esac

end=$(( $(date +%s) + seconds ))
round=0
while [ "$(date +%s)" -lt "$end" ]; do
  round=$((round + 1))
  echo "soak round=$round"
  scripts/stress-test.sh --connections "$connections" --concurrency "$concurrency" --bytes "$bytes"
done
