#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "${build_dir}"' EXIT

cc \
  -std=c11 \
  -Wall \
  -Wextra \
  -Werror \
  -I"${repo_root}/main" \
  "${repo_root}/main/playback_mix.c" \
  "${repo_root}/main/tests/test_playback_mix_host.c" \
  -o "${build_dir}/test_playback_mix"

"${build_dir}/test_playback_mix"
