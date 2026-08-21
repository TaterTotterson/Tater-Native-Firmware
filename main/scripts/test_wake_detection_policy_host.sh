#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
main_dir="$(cd "${script_dir}/.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "${build_dir}"' EXIT

cc -std=c11 -Wall -Wextra -Werror \
  -I"${main_dir}" \
  "${main_dir}/wake_detection_policy.c" \
  "${main_dir}/tests/test_wake_detection_policy_host.c" \
  -lm \
  -o "${build_dir}/test_wake_detection_policy"

"${build_dir}/test_wake_detection_policy"
