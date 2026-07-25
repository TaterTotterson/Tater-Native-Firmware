#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT="${TMPDIR:-/tmp}/tater-xvf3800-doa-test"

cc \
  -std=c11 \
  -Wall \
  -Wextra \
  -Werror \
  -I "${ROOT_DIR}" \
  "${ROOT_DIR}/tests/test_xvf3800_doa_host.c" \
  "${ROOT_DIR}/xvf3800_doa.c" \
  -o "${OUTPUT}"

"${OUTPUT}"
