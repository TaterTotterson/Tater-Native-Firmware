#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT="${TMPDIR:-/tmp}/tater-doa-estimator-test"

cc \
  -std=c11 \
  -Wall \
  -Wextra \
  -Werror \
  -I "${ROOT_DIR}/satellite-xmos-firmware/src/doa" \
  "${ROOT_DIR}/tests/test_doa_estimator_host.c" \
  "${ROOT_DIR}/satellite-xmos-firmware/src/doa/doa_estimator.c" \
  -o "${OUTPUT}"

"${OUTPUT}"
