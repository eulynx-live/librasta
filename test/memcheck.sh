#!/usr/bin/env bash
#
# Run all CTest-registered unit tests under Valgrind and fail if any memory
# leaks or memory errors (invalid reads/writes, use of uninitialised values,
# invalid frees, ...) are detected.
#
# Usage: test/memcheck.sh [options]
#
#   -b, --build-dir DIR   Build directory to use (default: <repo>/cmake-build-debug)
#   -n, --no-build        Do not configure/build, use an existing build directory
#   -R, --tests-regex RE  Only check tests whose name matches RE
#   -s, --strict          Also report "possibly lost" blocks as leaks
#   -j, --jobs N          Parallel build jobs (default: nproc)
#   -h, --help            Show this help
#
# Any arguments after "--" are passed to the CMake configure step, e.g.
#   test/memcheck.sh -- -DENABLE_RASTA_TLS=ON -DENABLE_RASTA_OPAQUE=ON
#
# Valgrind logs are written to <build-dir>/memcheck/<test>.log.
# Exit code: 0 if all tests are clean, 1 otherwise.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${REPO_DIR}/cmake-build-debug"
DO_BUILD=1
TEST_REGEX=""
LEAK_KINDS="definite,indirect"
JOBS="$(nproc 2>/dev/null || echo 4)"
CMAKE_EXTRA_ARGS=()

usage() {
    sed -n '3,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
    -b | --build-dir)
        BUILD_DIR="$(realpath -m "$2")"
        shift 2
        ;;
    -n | --no-build)
        DO_BUILD=0
        shift
        ;;
    -R | --tests-regex)
        TEST_REGEX="$2"
        shift 2
        ;;
    -s | --strict)
        LEAK_KINDS="definite,indirect,possible"
        shift
        ;;
    -j | --jobs)
        JOBS="$2"
        shift 2
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    --)
        shift
        CMAKE_EXTRA_ARGS=("$@")
        break
        ;;
    *)
        echo "Unknown option: $1" >&2
        usage >&2
        exit 2
        ;;
    esac
done

if [[ -t 1 ]]; then
    RED=$'\e[31m' GREEN=$'\e[32m' YELLOW=$'\e[33m' BOLD=$'\e[1m' RESET=$'\e[0m'
else
    RED="" GREEN="" YELLOW="" BOLD="" RESET=""
fi

for tool in valgrind cmake ctest python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "${RED}error:${RESET} '$tool' is required but not installed." >&2
        exit 2
    fi
done

# --- Build -------------------------------------------------------------------
if [[ $DO_BUILD -eq 1 ]]; then
    echo "${BOLD}==> Configuring (${BUILD_DIR})${RESET}"
    # Debug build with frame pointers gives readable valgrind stack traces
    cmake -S "$REPO_DIR" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_FLAGS="-fno-omit-frame-pointer" \
        -DBUILD_TESTING=ON \
        "${CMAKE_EXTRA_ARGS[@]}"
    echo "${BOLD}==> Building${RESET}"
    cmake --build "$BUILD_DIR" -j "$JOBS"
elif [[ ! -f "${BUILD_DIR}/CTestTestfile.cmake" ]]; then
    echo "${RED}error:${RESET} '${BUILD_DIR}' is not a configured build directory." >&2
    exit 2
fi

# --- Discover tests ------------------------------------------------------------
# Ask CTest for the registered tests and their commands, so newly added tests
# are picked up automatically. Output: one line per test, fields separated by
# the ASCII unit separator: name, working directory, command...
CTEST_ARGS=(--test-dir "$BUILD_DIR" --show-only=json-v1)
[[ -n "$TEST_REGEX" ]] && CTEST_ARGS+=(-R "$TEST_REGEX")

mapfile -t TESTS < <(ctest "${CTEST_ARGS[@]}" | python3 -c '
import json, sys
for t in json.load(sys.stdin).get("tests", []):
    if not t.get("command"):
        continue
    wd = next((p["value"] for p in t.get("properties", [])
               if p["name"] == "WORKING_DIRECTORY"), "")
    print("\x1f".join([t["name"], wd] + t["command"]))
')

if [[ ${#TESTS[@]} -eq 0 ]]; then
    echo "${RED}error:${RESET} no tests found in '${BUILD_DIR}'." >&2
    exit 2
fi

# --- Run tests under valgrind ----------------------------------------------------
LOG_DIR="${BUILD_DIR}/memcheck"
mkdir -p "$LOG_DIR"
VALGRIND_ERROR_CODE=99

VALGRIND_OPTS=(
    --leak-check=full
    --show-leak-kinds="$LEAK_KINDS"
    --errors-for-leak-kinds="$LEAK_KINDS"
    --track-origins=yes
    --error-exitcode="$VALGRIND_ERROR_CODE"
)

FAILED=()
for entry in "${TESTS[@]}"; do
    IFS=$'\x1f' read -r -a fields <<<"$entry"
    name="${fields[0]}"
    workdir="${fields[1]:-$BUILD_DIR}"
    cmd=("${fields[@]:2}")
    log="${LOG_DIR}/${name}.log"

    printf '%s==> %-45s%s ' "$BOLD" "$name" "$RESET"
    set +e
    (cd "$workdir" && valgrind "${VALGRIND_OPTS[@]}" --log-file="$log" "${cmd[@]}") \
        >"${LOG_DIR}/${name}.stdout" 2>&1
    rc=$?
    set -e

    if [[ $rc -eq 0 ]]; then
        echo "${GREEN}OK${RESET}"
        continue
    fi

    FAILED+=("$name")
    if [[ $rc -eq $VALGRIND_ERROR_CODE ]]; then
        echo "${RED}MEMORY ERRORS${RESET}"
    else
        echo "${YELLOW}TEST FAILED (exit code $rc)${RESET}"
        echo "    test output: ${LOG_DIR}/${name}.stdout"
    fi
    # Print the valgrind error and leak summary of the failing test
    grep -E '(ERROR SUMMARY|definitely lost|indirectly lost|possibly lost):' "$log" |
        sed 's/^==[0-9]*== /    /' || true
    echo "    valgrind log: $log"
done

# Show full valgrind output in CI so the report is visible in the job log
if [[ ${#FAILED[@]} -gt 0 && -n "${CI:-}" ]]; then
    for name in "${FAILED[@]}"; do
        echo
        echo "${BOLD}----- valgrind log: ${name} -----${RESET}"
        cat "${LOG_DIR}/${name}.log"
    done
fi

echo
if [[ ${#FAILED[@]} -gt 0 ]]; then
    echo "${RED}${BOLD}${#FAILED[@]} of ${#TESTS[@]} test(s) failed memcheck:${RESET} ${FAILED[*]}"
    exit 1
fi
echo "${GREEN}${BOLD}All ${#TESTS[@]} test(s) are free of memory leaks and errors.${RESET}"
