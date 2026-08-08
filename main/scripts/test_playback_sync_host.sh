#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
main_dir="$(cd "${script_dir}/.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "${build_dir}"' EXIT

cc -std=c11 -Wall -Wextra -Werror \
  -I"${main_dir}" \
  "${main_dir}/playback_sync.c" \
  "${main_dir}/tests/test_playback_sync_host.c" \
  -o "${build_dir}/test_playback_sync"

"${build_dir}/test_playback_sync"
