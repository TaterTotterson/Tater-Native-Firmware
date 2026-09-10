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
  "${ROOT_DIR}/wake_model_policy.c" \
  "${ROOT_DIR}/tests/test_wake_model_policy_host.c" \
  -o "${BUILD_DIR}/test_wake_model_policy_host"

"${BUILD_DIR}/test_wake_model_policy_host"
