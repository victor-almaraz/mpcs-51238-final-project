#!/usr/bin/env bash
# Times offline renders of examples/straintest by thread count.
#
# Run from anywhere in the repository; see --help.

set -euo pipefail

usage() {
    cat <<'EOF'
usage: scripts/straintest.sh [-t "1 2 4 8 16"] [-r N] [-s S] [-b DIR] [--no-build] [-h]

Renders examples/straintest offline with espresso-server on each thread
count and prints, for each count, the mean, median, fastest, and slowest
wall time of a render, the mean less the setup time, the mean's speedup
over one thread, and how many times faster than real time the mean
render runs.

The setup time is what a render spends outside its blocks: the process's
start, reading the IR, and building the plan. Each round also times a
render of 0.01 s on one thread, and the median of those renders is the
setup time.

The renders alternate thread counts, round by round, so every count sees
the same machine; one short render before the first round warms the
caches. Every render is compared with the first: the output is the same
at every thread count, and the script exits non-zero when one differs.

The table goes to standard output, after comment lines (#) that name the
machine and the build; progress goes to standard error on a terminal.

options:
  -t LIST      thread counts, quoted and separated by spaces (default "1 2 4 8 16")
  -r N         renders per thread count (default 5)
  -s S         seconds of audio per render (default 10)
  -b DIR       build tree (default build-release). The server target is
               built first; a tree that does not exist is configured as
               a Release build of the server alone
  --no-build   use the tree's espresso-server as it is
  -h, --help   show this help
EOF
}

. "$(dirname "$0")/common.sh"

threads="1 2 4 8 16"
runs=5
seconds=10
build=build-release
compile=1
while [ $# -gt 0 ]; do
    case "$1" in
    -t | -r | -s | -b)
        [ $# -ge 2 ] || die "$1 needs a value"
        case "$1" in
        -t) threads=$2 ;;
        -r) runs=$2 ;;
        -s) seconds=$2 ;;
        -b) build=$2 ;;
        esac
        shift
        ;;
    --no-build) compile=0 ;;
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

whole() { case "$1" in '' | *[!0-9]*) return 1 ;; esac; }
whole "$runs" && [ "$runs" -ge 1 ] || die "-r needs a whole number of at least 1, got '$runs'"
case "$seconds" in '' | *[!0-9.]* | *.*.* | .) die "-s needs a number of seconds, got '$seconds'" ;; esac
[ -n "$threads" ] || die "-t needs at least one thread count"
for t in $threads; do
    whole "$t" && [ "$t" -ge 1 ] || die "-t needs whole numbers of at least 1, got '$t'"
done

ir=examples/straintest/straintest.ir.json
[ -f "$ir" ] || die "no $ir"
server=$build/bin/espresso-server

if [ "$compile" = 1 ]; then
    if [ ! -f "$build/CMakeCache.txt" ]; then
        # shellcheck disable=SC2046 # one argument per line of shared_sources
        cmake -B "$build" -S . -DCMAKE_BUILD_TYPE=Release -DESPRESSO_BUILD_CLIENT=OFF \
            -DESPRESSO_BUILD_TESTS=OFF -DFETCHCONTENT_BASE_DIR="$root/$build/_deps" \
            $(shared_sources) >&2
    fi
    cmake --build "$build" --target espresso_server -j "$(cpu_count)" >&2
fi
[ -x "$server" ] || die "no $server; build it, or drop --no-build"

type=$(sed -n 's/^CMAKE_BUILD_TYPE:[A-Z]*=//p' "$build/CMakeCache.txt" 2>/dev/null || true)
[ "$type" = Release ] || echo "straintest.sh: $build is a ${type:-unknown} build; its times are not a Release build's" >&2

# The processor's name, and its physical cores where the system reports them.
cpu_name() {
    if [ "$(uname)" = Darwin ]; then
        sysctl -n machdep.cpu.brand_string 2>/dev/null && return
    fi
    if [ -r /proc/cpuinfo ]; then
        local name
        name=$(sed -n 's/^model name[[:space:]]*: //p' /proc/cpuinfo | head -n 1)
        [ -n "$name" ] && echo "$name" && return
    fi
    if command -v lscpu >/dev/null; then
        lscpu | sed -n 's/^Model name:[[:space:]]*//p' | head -n 1
        return
    fi
    echo unknown
}
physical_cores() {
    if [ "$(uname)" = Darwin ]; then
        sysctl -n hw.physicalcpu 2>/dev/null && return
    fi
    if command -v lscpu >/dev/null; then
        lscpu -p=core,socket 2>/dev/null | grep -v '^#' | sort -u | wc -l | tr -d ' '
        return
    fi
    echo unknown
}

work=$(mktemp -d "${TMPDIR:-/tmp}/straintest.XXXXXX")
trap 'rm -rf "$work"' EXIT

# Renders S seconds on N threads to FILE and prints the wall time in seconds.
render() {
    local n=$1 s=$2 out=$3 elapsed
    local TIMEFORMAT=%R
    if ! elapsed=$({ time "$server" --render "$ir" --out "$out" --seconds "$s" --threads "$n" \
        >/dev/null 2>"$work/errors"; } 2>&1); then
        sed '/^usage:/,$d' "$work/errors" >&2  # the error, without the usage after it
        die "the render on $n threads failed"
    fi
    echo "$elapsed"
}

progress() { if [ -t 2 ]; then printf '\r%-60s' "$*" >&2; fi; }

counts=($threads)
total=$((runs * (${#counts[@]} + 1)))
progress "warming up"
render "${counts[0]}" 1 "$work/warm.wav" >/dev/null
done_count=0
for round in $(seq 1 "$runs"); do
    done_count=$((done_count + 1))
    progress "render $done_count of $total: setup, round $round"
    elapsed=$(render 1 0.01 "$work/setup.wav") || exit 1
    echo "setup $elapsed" >>"$work/times"
    for n in "${counts[@]}"; do
        done_count=$((done_count + 1))
        progress "render $done_count of $total: $n threads, round $round"
        out=$work/render.wav
        [ -f "$work/first.wav" ] || out=$work/first.wav
        elapsed=$(render "$n" "$seconds" "$out") || exit 1
        echo "$n $elapsed" >>"$work/times"
        if [ "$out" != "$work/first.wav" ] && ! cmp -s "$out" "$work/first.wav"; then
            if [ -t 2 ]; then echo >&2; fi
            die "the render on $n threads in round $round differs from the first render (${counts[0]} threads)"
        fi
    done
done
if [ -t 2 ]; then printf '\r%-60s\r' "" >&2; fi

echo "# straintest: $seconds s of audio, $runs renders per thread count, alternated"
echo "# machine: $(uname -sm), $(cpu_name), $(physical_cores) physical cores, $(getconf _NPROCESSORS_ONLN 2>/dev/null || echo unknown) logical CPUs"
echo "# build: $server (${type:-unknown}), $(git rev-parse --short HEAD)$(git diff --quiet HEAD -- || echo ' with changes')"
echo "# every render is the same as the first"
awk -v order="$threads" -v seconds="$seconds" '
    { times[$1] = times[$1] " " $2 }
    # Sorts the k values of v in place: insertion sort, as k is the run
    # count, a handful.
    function sorted(v, k,    a, b, x) {
        for (a = 2; a <= k; ++a) {
            x = v[a] + 0
            for (b = a - 1; b >= 1 && v[b] + 0 > x; --b) v[b + 1] = v[b]
            v[b + 1] = x
        }
    }
    function median(v, k) { return (k % 2) ? v[(k + 1) / 2] : (v[k / 2] + v[k / 2 + 1]) / 2 }
    END {
        k = split(times["setup"], v, " ")
        sorted(v, k)
        setup = median(v, k)
        printf "# setup: %.3f s per render (process start, IR read, plan build)\n", setup
        printf "%-8s %5s %9s %9s %9s %9s %9s %8s %9s\n", "threads", "runs", "mean_s", "median_s", "min_s", "max_s", "render_s", "speedup", "realtime"
        n = split(order, list, " ")
        for (i = 1; i <= n; ++i) {
            t = list[i]
            if (t in seen) continue
            seen[t] = 1
            k = split(times[t], v, " ")
            sorted(v, k)
            sum = 0
            for (a = 1; a <= k; ++a) sum += v[a]
            mean[t] = sum / k
            row[i] = sprintf("%-8s %5d %9.3f %9.3f %9.3f %9.3f %9.3f", t, k, mean[t], median(v, k), v[1], v[k], mean[t] - setup)
            key[i] = t
        }
        base = ("1" in mean) ? mean["1"] : mean[list[1]]
        for (i = 1; i <= n; ++i) {
            if (!(i in row)) continue
            printf "%s %7.2fx %8.2fx\n", row[i], base / mean[key[i]], seconds / mean[key[i]]
        }
    }' "$work/times"
