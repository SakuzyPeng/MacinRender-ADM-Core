#!/usr/bin/env bash
# Record actual compiler commands and CMake's resolved dependency sources.
# usage: build-info.sh <build-dir> <out-file>
set -euo pipefail
script_dir="$(cd "$(dirname "$0")" && pwd)"
python_bin="${PYTHON:-}"
if [ -z "$python_bin" ]; then
    for candidate in python3 python; do
        if command -v "$candidate" >/dev/null 2>&1; then
            python_bin="$candidate"
            break
        fi
    done
fi
if [ -z "$python_bin" ]; then
    echo "error: Python 3 is required to read the build record" >&2
    exit 2
fi
exec "$python_bin" "${script_dir}/build_info.py" "$@"
