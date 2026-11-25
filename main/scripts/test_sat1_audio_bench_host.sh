#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
"${PYTHON_BIN:-python3}" "$ROOT/main/tests/test_sat1_audio_bench_host.py"
