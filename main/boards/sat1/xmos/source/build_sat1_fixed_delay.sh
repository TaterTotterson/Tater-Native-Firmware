#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
XTC_DIR="${XMOS_TOOL_PATH:-/Applications/XMOS_XTC_15.3.1}"
CMAKE_BIN="${CMAKE_BIN:-/opt/homebrew/opt/cmake/bin/cmake}"
NINJA_BIN="${NINJA_BIN:-/opt/homebrew/opt/ninja/bin/ninja}"
PYTHON_BIN="${PYTHON_BIN:-/opt/homebrew/bin/python3.11}"
VENV_DIR="${SAT1_XMOS_VENV_DIR:-${ROOT_DIR}/.venv}"
PIPELINE="${SAT1_XMOS_PIPELINE:-beam_ic}"
BENCH="${SAT1_XMOS_AUDIO_BENCH:-0}"
case "$BENCH" in
  0|1) ;;
  *) echo "SAT1_XMOS_AUDIO_BENCH must be 0 or 1" >&2; exit 1 ;;
esac
BUILD_SUFFIX=""
if [[ "$BENCH" == "1" ]]; then BUILD_SUFFIX="_bench"; fi
BUILD_DIR="${ROOT_DIR}/build_${PIPELINE}${BUILD_SUFFIX}"
case "${PIPELINE}" in
  beam_ic) FW_VERSION=v1.2.0-dev.8 ;;
  legacy) FW_VERSION=v1.2.0-dev.1 ;;
  aec4) FW_VERSION=v1.2.0-dev.2 ;;
  aec4_beam) FW_VERSION=v1.2.0-dev.3 ;;
  *) echo "Unknown SAT1_XMOS_PIPELINE: ${PIPELINE}" >&2; exit 1 ;;
esac
if [[ "$BENCH" == "1" ]]; then
  case "$PIPELINE" in
    beam_ic) FW_VERSION=v1.2.0-dev.9 ;;
    legacy) FW_VERSION=v1.2.0-dev.12 ;;
    aec4) FW_VERSION=v1.2.0-dev.10 ;;
    aec4_beam) FW_VERSION=v1.2.0-dev.11 ;;
  esac
fi
TARGET="${SAT1_XMOS_TARGET:-satellite1_firmware_fixed_delay}"
if [[ "$BENCH" == "1" && "$TARGET" != satellite1_firmware_fixed_delay ]]; then
  echo "Audio bench mode requires satellite1_firmware_fixed_delay" >&2
  exit 1
fi
CLEAN_BUILD="${XMOS_CLEAN_BUILD:-1}"

if [[ ! -x "${XTC_DIR}/bin/xcc" ]]; then
  echo "XTC Tools not found at ${XTC_DIR}."
  echo "Install XMOS XTC Tools 15.3.1, or set XMOS_TOOL_PATH to its install directory."
  exit 1
fi

if [[ ! -x "${CMAKE_BIN}" ]]; then
  echo "CMake not found at ${CMAKE_BIN}."
  exit 1
fi

if [[ ! -x "${NINJA_BIN}" ]]; then
  echo "Ninja not found at ${NINJA_BIN}."
  exit 1
fi

if [[ ! -x "${VENV_DIR}/bin/python" ]]; then
  "${PYTHON_BIN}" -m venv "${VENV_DIR}"
fi

"${VENV_DIR}/bin/python" -m pip install -r "${ROOT_DIR}/requirements.txt"

export XMOS_TOOL_PATH="${XTC_DIR}"
export XMOS_HOME="${XMOS_HOME:-${HOME}/.xmos}"
export PATH="${VENV_DIR}/bin:${XTC_DIR}/bin:${XTC_DIR}/libexec:${PATH}"
export LD_LIBRARY_PATH="${XTC_DIR}/lib:${LD_LIBRARY_PATH:-}"
export DYLD_LIBRARY_PATH="${XTC_DIR}/lib:${DYLD_LIBRARY_PATH:-}"
export XCC_C_INCLUDE_PATH="${XTC_DIR}/target/include:${XTC_DIR}/target/include/clang"
export XCC_XC_INCLUDE_PATH="${XTC_DIR}/target/include/xc:${XCC_C_INCLUDE_PATH}"
export XCC_CPLUS_INCLUDE_PATH="${XCC_C_INCLUDE_PATH}:${XTC_DIR}/target/include/c++/v1"
export XCC_ASSEMBLER_INCLUDE_PATH="${XCC_C_INCLUDE_PATH}"
export XCC_LIBRARY_PATH="${XTC_DIR}/target/lib"
export XCC_DEVICE_PATH="${XTC_DIR}/configs:${XTC_DIR}/configs/.deprecated"
export XCC_TARGET_PATH="${XMOS_HOME}/targets:${XTC_DIR}/targets:${XTC_DIR}/targets/.deprecated"
export XCC_EXEC_PREFIX="${XTC_DIR}/libexec/"
export XMOS_DOC_PATH="${XTC_DIR}/doc"
export PYTHON_HOME="${XTC_DIR}/lib/jython"
export PYTHONPATH="${PYTHONPATH:-}:${XTC_DIR}/lib/python"
export PYTHON_VERBOSE="warning"
export LC_ALL=C
export XMOS_CACHE_PATH="${XMOS_HOME}/cache"
export XMOS_REPO_PATH="${XMOS_HOME}/repos"
export XMOS_MAKE_PATH="${XTC_DIR}/build"
export XMOS_CMAKE_PATH="${XTC_DIR}/build/xcommon_cmake"

if [[ "${CLEAN_BUILD}" != "0" ]]; then
  rm -rf "${BUILD_DIR}"
fi

"${CMAKE_BIN}" \
  -G Ninja \
  -B "${BUILD_DIR}" \
  -S "${ROOT_DIR}" \
  --toolchain "${ROOT_DIR}/xmos_cmake_toolchain/xs3a.cmake" \
  -DCMAKE_MAKE_PROGRAM="${NINJA_BIN}" \
  -DPython3_EXECUTABLE="${VENV_DIR}/bin/python" \
  -DSAT1_XMOS_PIPELINE="${PIPELINE}" \
  -DSAT1_XMOS_VERSION="${FW_VERSION}" \
  -DSAT1_AUDIO_BENCH="${BENCH}" \
  -DBUILD_VOICE_COMPAT_TESTS=ON \
  -DFPH_AEC_4Y_TESTS=ON

"${CMAKE_BIN}" --build "${BUILD_DIR}" --target "create_flash_img_${TARGET}"

echo "Built ${BUILD_DIR}/${TARGET}.factory.bin"
echo "MD5  ${BUILD_DIR}/${TARGET}.factory.md5"

if [[ "${SAT1_XMOS_RUN_SIM_TESTS:-0}" == "1" ]]; then
  if [[ "${PIPELINE}" == "legacy" || "${PIPELINE}" == "beam_ic" ]]; then
    SIM_TARGETS=(voice_compat_test)
  else
    SIM_TARGETS=(fph_aec_4y_init_canary_test sat1_aec_beam_test)
  fi
  "${CMAKE_BIN}" --build "${BUILD_DIR}" --target "${SIM_TARGETS[@]}"
  for SIM_TARGET in "${SIM_TARGETS[@]}"; do
    SIM_IMAGE="$(find "${BUILD_DIR}" -name "${SIM_TARGET}.xe" -print -quit)"
    "${XTC_DIR}/bin/xsim" "${SIM_IMAGE}"
  done
fi
