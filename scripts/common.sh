# Definitions the repository's scripts share. Sourced, not run:
#
#   . "$(dirname "$0")/common.sh"
#
# It moves to the repository's root, which it sets as $root, and defines
# die, cpu_count, and shared_sources.

# Prints "SCRIPT: MESSAGE" on standard error and exits with status 1.
die() {
    echo "$(basename "$0"): $*" >&2
    exit 1
}

root=$(git rev-parse --show-toplevel 2>/dev/null) || die "run inside the repository"
cd "$root" || die "cannot enter $root"

# The number of processors online, or 4 when the system does not say.
cpu_count() {
    getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4
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
