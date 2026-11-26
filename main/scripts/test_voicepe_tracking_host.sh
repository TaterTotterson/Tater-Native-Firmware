#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "${BUILD_DIR}"' EXIT
XMOS_DIR="${ROOT_DIR}/boards/voice_pe/xmos"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DVOICEPE_DOA_HOST_TEST=1 \
  -I "${ROOT_DIR}" -I "${XMOS_DIR}" -I "${XMOS_DIR}/source/src/ffva/src" \
  "${ROOT_DIR}/tests/test_voicepe_tracking_host.c" \
  "${ROOT_DIR}/boards/voice_pe/voicepe_audio_hints.c" \
  "${XMOS_DIR}/source/src/ffva/src/doa/doa_estimator.c" \
  -o "${BUILD_DIR}/test_voicepe_tracking"
"${BUILD_DIR}/test_voicepe_tracking"
