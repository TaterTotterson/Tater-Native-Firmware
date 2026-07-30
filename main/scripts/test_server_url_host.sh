#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "${BUILD_DIR}"' EXIT

cc \
  -std=c11 \
  -Wall \
  -Wextra \
  -Werror \
  -I "${ROOT_DIR}" \
  "${ROOT_DIR}/server_url.c" \
  "${ROOT_DIR}/tests/test_server_url_host.c" \
  -o "${BUILD_DIR}/test_server_url_host"

"${BUILD_DIR}/test_server_url_host"
