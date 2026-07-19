#!/bin/sh
set -eu
exec make mini BUILD_DIR="${BUILD_DIR:-build/mini}"
