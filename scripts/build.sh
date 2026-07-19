#!/bin/sh
set -eu
make build BUILD_DIR="${BUILD_DIR:-build}" BUILD_TYPE="${BUILD_TYPE:-Release}"
make test BUILD_DIR="${BUILD_DIR:-build}" BUILD_TYPE="${BUILD_TYPE:-Release}"
