#!/usr/bin/env sh
set -eu

configuration="${1:-Debug}"
case "$configuration" in
  Debug|Release) ;;
  *) echo "usage: $0 [Debug|Release]" >&2; exit 2 ;;
esac

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
project_dir="$repo_root/plugins/py_host/python"
target_dir="$repo_root/run/$configuration/lib/python3.12/site-packages"
build_name=$(printf '%s' "$configuration" | tr '[:upper:]' '[:lower:]')
python_exe="$repo_root/out/build/linux-x64-$build_name/vcpkg_installed/x64-linux/tools/python3/python3"
requirements=$(mktemp)
trap 'rm -f "$requirements"' EXIT

if [ ! -x "$python_exe" ]; then
  echo "vcpkg Python not found: $python_exe (configure/build the project first)" >&2
  exit 1
fi

uv export --project "$project_dir" --frozen --no-dev --no-emit-project \
  --format requirements-txt --output-file "$requirements"
uv pip sync --python "$python_exe" --target "$target_dir" \
  --allow-empty-requirements "$requirements"
