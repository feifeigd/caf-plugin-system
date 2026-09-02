#!/usr/bin/env sh
set -eu

configuration="${1:-Debug}"
case "$configuration" in
  Debug|Release) ;;
  *) echo "usage: $0 [Debug|Release]" >&2; exit 2 ;;
esac

build_name=$(printf '%s' "$configuration" | tr '[:upper:]' '[:lower:]')
cmake --build --preset "linux-x64-$build_name" --target sync_python_deps
