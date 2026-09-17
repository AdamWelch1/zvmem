#!/usr/bin/env bash
# zvmemd launcher: creates a venv on first run, installs deps, serves.
# Uses Python 3.12 by default (override with ZVMEMD_PYTHON).
# Usage: ./run.sh            (config via ZVMEMD_* env vars, see README.md)
set -euo pipefail
cd "$(dirname "$0")"

PYTHON="${ZVMEMD_PYTHON:-python3.12}"
if ! command -v "$PYTHON" >/dev/null 2>&1; then
  echo "zvmemd: '$PYTHON' not found on PATH — install Python 3.12 or set ZVMEMD_PYTHON to an available interpreter" >&2
  exit 1
fi

if [ ! -d .venv ]; then
  echo "zvmemd: creating virtualenv in .venv/ with $("$PYTHON" --version 2>&1)"
  "$PYTHON" -m venv .venv
fi

./.venv/bin/pip install --quiet --upgrade pip
./.venv/bin/pip install --quiet -r requirements.txt

exec ./.venv/bin/python -m zvmemd "$@"
