#!/usr/bin/env bash
# Regenerates the README badges from a fresh run of the test suite.
#
# Run from anywhere in the repository; see --help.

set -euo pipefail

usage() {
    cat <<'EOF'
usage: scripts/badges.sh [--tests | --coverage] [-j N] [-h]

Builds the project, runs the test suite, and rewrites the badges:

  docs/badges/tests.svg      pass count, from the build tree build/
  docs/badges/coverage.svg   line coverage of src/ and include/, from the
                             build tree build-coverage/ (Clang, PROJ_COVERAGE=ON)

It also adds any missing badge line under the README's title. A tree that
does not exist yet is configured first; dependency sources already in
third_party/ are reused. The script exits non-zero when a build or a test
fails, and leaves the coverage badge unchanged in that case.

options:
  --tests      only the test badge
  --coverage   only the coverage badge
  -j N         parallel build jobs (default: the number of CPUs)
  -h, --help   show this help

environment:
  LLVM_BIN     directory holding clang, clang++, llvm-profdata, and llvm-cov
               for the coverage tree (default: Homebrew's LLVM on macOS,
               else the directory of clang++ on PATH)
EOF
}

die() {
    echo "badges.sh: $*" >&2
    exit 1
}

root=$(git rev-parse --show-toplevel 2>/dev/null) || die "run inside the repository"
cd "$root"

tests=1
coverage=1
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
while [ $# -gt 0 ]; do
    case "$1" in
    --tests) coverage=0 ;;
    --coverage) tests=0 ;;
    -j)
        [ $# -ge 2 ] || die "-j needs a number"
        jobs=$2
        shift
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    *)
        usage >&2
        exit 2
        ;;
    esac
    shift
done
case "$jobs" in '' | *[!0-9]*) die "-j needs a number, got '$jobs'" ;; esac

# Finds the LLVM toolchain for the coverage tree.
llvm_bin() {
    if [ -n "${LLVM_BIN:-}" ]; then
        echo "$LLVM_BIN"
        return
    fi
    for dir in /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin; do
        if [ -x "$dir/clang++" ]; then
            echo "$dir"
            return
        fi
    done
    local cxx
    cxx=$(command -v clang++) || die "no clang++ found; set LLVM_BIN"
    dirname "$cxx"
}

# -DFETCHCONTENT_SOURCE_DIR_<NAME>=... for each dependency already
# downloaded into third_party/, so a new tree does not fetch it again.
shared_sources() {
    local src name
    for src in third_party/*-src; do
        [ -d "$src" ] || continue
        name=$(basename "$src" -src | tr '[:lower:]' '[:upper:]')
        echo "-DFETCHCONTENT_SOURCE_DIR_${name}=$root/$src"
    done
}

# Adds "![label](docs/badges/label.svg)" under the title unless present.
# Badges keep the order tests, coverage.
readme_badge() {
    local label=$1 line
    line="![$label](docs/badges/$label.svg)"
    grep -qxF "$line" README.md && return
    awk -v line="$line" '
        /^!\[[a-z]+\]\(docs\/badges\// { last = NR }
        { lines[NR] = $0 }
        END {
            for (i = 1; i <= NR; ++i) {
                print lines[i]
                if (i == (last ? last : 1)) {
                    if (!last) print ""
                    print line
                }
            }
        }' README.md >README.md.tmp
    mv README.md.tmp README.md
}

if [ "$tests" = 1 ]; then
    [ -f build/CMakeCache.txt ] || cmake -B build -S . -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    cmake --build build -j "$jobs"
    cmake --build build --target test-badge
    readme_badge tests
fi

if [ "$coverage" = 1 ]; then
    if [ ! -f build-coverage/CMakeCache.txt ]; then
        bin=$(llvm_bin)
        sysroot=()
        if [ "$(uname)" = Darwin ]; then
            sysroot=(-DCMAKE_OSX_SYSROOT="$(xcrun --show-sdk-path)")
        fi
        # shellcheck disable=SC2046 # one argument per line of shared_sources
        cmake -B build-coverage -S . \
            -DCMAKE_C_COMPILER="$bin/clang" -DCMAKE_CXX_COMPILER="$bin/clang++" \
            -DPROJ_COVERAGE=ON -DFETCHCONTENT_BASE_DIR="$root/build-coverage/_deps" \
            ${sysroot[@]+"${sysroot[@]}"} $(shared_sources)
    fi
    cmake --build build-coverage -j "$jobs"
    cmake --build build-coverage --target coverage-badge
    readme_badge coverage
fi

sed -n 's:.*<title>\(.*\)</title>.*:\1:p' docs/badges/*.svg
