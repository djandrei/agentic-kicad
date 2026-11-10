#!/usr/bin/env bash
#
# Agentic-KiCad stage gate: build, agentic tests, upstream suites, formatting.
#
# Usage: tools/agentic/stage-gate.sh [--upstream-suites "qa_pcbnew qa_eeschema ..."] [--off-build]
#
#   --upstream-suites  upstream ctest suites to compare against doc/agentic/baseline-tests.txt
#   --off-build        also configure and build build/upstream with KICAD_AGENTIC=OFF
#
# Exits non-zero on the first failing step.

set -euo pipefail

ROOT="$( cd "$( dirname "${BASH_SOURCE[0]}" )/../.." && pwd )"
BUILD_ON="${ROOT}/build/agentic"
BUILD_OFF="${ROOT}/build/upstream"
JOBS="${JOBS:-$( nproc )}"
UPSTREAM_SUITES=""
OFF_BUILD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --upstream-suites) UPSTREAM_SUITES="$2"; shift 2 ;;
        --off-build)       OFF_BUILD=1; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

COMMON_FLAGS=(
    -G Ninja
    -DCMAKE_BUILD_TYPE=QABUILD
    -DKICAD_STDLIB_LIGHT_DEBUG=ON
    -DKICAD_BUILD_QA_TESTS=ON
    -DKICAD_USE_CMAKE_FINDPROTOBUF=ON
    -DCMAKE_C_COMPILER_LAUNCHER=ccache
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
)

step() { echo; echo "=== stage-gate: $* ==="; }

# Let ccache share objects between build/upstream and build/agentic: paths relative to the
# source root, precompiled headers accepted, and room for two debug builds.
export CCACHE_BASEDIR="${CCACHE_BASEDIR:-${ROOT}}"
export CCACHE_SLOPPINESS="${CCACHE_SLOPPINESS:-pch_defines,time_macros,include_file_mtime,include_file_ctime}"
export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-40G}"

# The RHI shader compilers (glslang-tools, spirv-tools) can be unpacked without root into
# build/hosttools (apt-get download + dpkg -x) when the system packages are absent.
if ! command -v glslangValidator >/dev/null && [[ -x "${ROOT}/build/hosttools/usr/bin/glslangValidator" ]]; then
    export VULKAN_SDK="${ROOT}/build/hosttools/usr"
fi

step "build ${BUILD_ON} (KICAD_AGENTIC=ON)"
if [[ ! -f "${BUILD_ON}/build.ninja" ]]; then
    cmake -S "${ROOT}" -B "${BUILD_ON}" "${COMMON_FLAGS[@]}" -DKICAD_AGENTIC=ON
fi
ninja -C "${BUILD_ON}" -j "${JOBS}"

step "agentic tests"
ctest --test-dir "${BUILD_ON}" -R '^qa_agentic' --output-on-failure

if [[ -n "${UPSTREAM_SUITES}" ]]; then
    step "upstream suites against baseline: ${UPSTREAM_SUITES}"
    SUITES_FAILED=0
    for suite in ${UPSTREAM_SUITES}; do
        "${ROOT}/tools/agentic/compare-baseline.py" compare "${ROOT}/doc/agentic/baseline-tests.txt" \
            "${BUILD_ON}/qa" "${suite}" || SUITES_FAILED=1
    done

    # Upstream tests write their settings back into the tracked qa/data/config files.  Restore
    # them, pass or fail, so the working tree (and the formatting check) sees only real changes.
    git -C "${ROOT}" checkout -- qa/data/config

    if [[ "${SUITES_FAILED}" == 1 ]]; then
        echo "upstream suites failed against the baseline (see above)" >&2
        exit 1
    fi
fi

if [[ "${OFF_BUILD}" == 1 ]]; then
    step "build ${BUILD_OFF} (KICAD_AGENTIC=OFF)"
    if [[ ! -f "${BUILD_OFF}/build.ninja" ]]; then
        cmake -S "${ROOT}" -B "${BUILD_OFF}" "${COMMON_FLAGS[@]}" -DKICAD_AGENTIC=OFF
    fi
    ninja -C "${BUILD_OFF}" -j "${JOBS}"
fi

step "formatting of fork commits"
BASE="$( git -C "${ROOT}" merge-base HEAD master )"
"${ROOT}/tools/check_coding.sh" --diff --ci --commit "${BASE}" || {
    echo "formatting differences found (see above)" >&2
    exit 1
}

echo
echo "=== stage-gate: PASSED ==="
