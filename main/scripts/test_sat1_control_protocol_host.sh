#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT="$(mktemp "${TMPDIR:-/tmp}/sat1-control.XXXXXX")"
trap 'rm -f "${OUTPUT}"' EXIT
cc -std=c11 -Wall -Wextra -Werror -I "${ROOT_DIR}" \
  "${ROOT_DIR}/boards/sat1/sat1_control_protocol.c" \
  "${ROOT_DIR}/tests/test_sat1_control_protocol_host.c" -o "${OUTPUT}"
"${OUTPUT}"
