#!/usr/bin/env bash
# Run at the repository root in a clean checkout. BASE is a Git revision.
set -euo pipefail
base=${1:?usage: bash ci/portable.sh BASE}
if [[ -n "$(git status --porcelain)" ]]; then
  echo "Portable qualification requires a clean checkout." >&2
  exit 1
fi
printf 'Commit: '; git rev-parse HEAD
git diff --check "$base" HEAD
python3 - <<'PY'
import ast
import subprocess
from pathlib import Path
files = subprocess.check_output(["git", "ls-files", "-z", "*.py"]).decode().split("\0")
for name in filter(None, files):
    ast.parse(Path(name).read_bytes(), filename=name)
PY
python3 prototypes/f12/test_history.py
python3 ci/native-inventory.py
python3 ci/test-native-gate.py
bash ci/test-public-fixtures.sh
make audit-transfer audit-sanitize fuzz-parsers
