#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "${BUILD_DIR}"' EXIT

cc \
  -std=c11 -Wall -Wextra -Werror -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "${ROOT_DIR}/tests/xmos_update_host_stubs" \
  -I "${ROOT_DIR}" \
  "${ROOT_DIR}/xmos_update.c" \
  "${ROOT_DIR}/tests/test_xmos_update_host.c" \
  -o "${BUILD_DIR}/test_xmos_update_host"

"${BUILD_DIR}/test_xmos_update_host"
