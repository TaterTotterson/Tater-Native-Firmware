#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CC="${CC:-clang}" "${PYTHON_BIN:-python3}" "$ROOT/main/tests/test_sat1_audio_bench_host.py"
