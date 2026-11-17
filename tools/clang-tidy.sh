#!/bin/sh
# Runs clang-tidy, with the checks .clang-tidy lists, over Phazite's own files (src/ and tests/; not external/, nor the
# two files that are mostly others' code: Rubber Band's single-file build, and the resampler, whose analysis lands in
# r8brain's headers) in a build's compile_commands.json, several at once. Exits 1 if any file has a
# finding, 0 if none, 2 if it could not run.
#   sh tools/clang-tidy.sh <build folder> [<files' pattern>]
#   sh tools/clang-tidy.sh <build folder> --since <commit>
# The build folder is configured without precompiled headers (-DPHAZITE_PRECOMPILED_HEADERS=OFF: clang-tidy can't read a
# compiler's own) and built far enough for its generated headers (the phazite_data target). The pattern (an extended
# regular expression) picks some files only; --since picks those that changed since the commit this one and that one both
# come from (uncommitted changes too): each .cpp, and for a header the .cpp files of its name, where what it declares is
# defined. What only includes a changed header isn't picked: the whole pass, which takes more than half an hour on four
# processors, is the nightly workflow's, and CI's Clang job runs --since main (.github/workflows). CLANG_TIDY names the
# clang-tidy to run (default: clang-tidy on the PATH);
# JOBS how many at once (default: the processors). On Windows, an MSVC build's database works with the clang-tidy of VS
# Code's C++ extension (CLANG_TIDY=~/.vscode/extensions/ms-vscode.cpptools-*/LLVM/bin/clang-tidy.exe).
set -u

build=${1:-}
pattern=${2:-.}
since=

if [ "${2:-}" = "--since" ]; then
    since=${3:-}
    pattern=.

    if [ -z "$since" ]; then
        build=
    fi
fi

if [ -z "$build" ] || [ ! -f "$build/compile_commands.json" ]; then
    echo "usage: sh tools/clang-tidy.sh <build folder with compile_commands.json> [<files' pattern> | --since <commit>]" >&2
    exit 2
fi

tidy=${CLANG_TIDY:-clang-tidy}
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}

if ! "$tidy" --version >/dev/null 2>&1; then
    echo "tools/clang-tidy.sh: $tidy doesn't run; set CLANG_TIDY" >&2
    exit 2
fi

if grep -q -e '/Yu' -e 'cmake_pch' "$build/compile_commands.json"; then
    echo "tools/clang-tidy.sh: $build uses precompiled headers; configure it with -DPHAZITE_PRECOMPILED_HEADERS=OFF" >&2
    exit 2
fi

list=$(mktemp)
changed=$(mktemp)
trap 'rm -f "$list" "$changed"' EXIT

# Each entry's "file" (with forward slashes) of Phazite's own C++ files: those in the repository's src/ and tests/.
root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel 2>/dev/null) || {
    echo "tools/clang-tidy.sh: not in a git clone" >&2
    exit 2
}

sed -n 's/^ *"file": "\(.*\)",\{0,1\}$/\1/p' "$build/compile_commands.json" | sed 's/\\\\/\//g' |
    grep -F -e "$root/src/" -e "$root/tests/" | grep -E '\.cpp$' | grep -v -e 'RubberBandLibrary.cpp$' -e 'Resampler.cpp$' |
    grep -E -e "$pattern" | sort -u > "$list"

if [ -n "$since" ]; then
    base=$(git -C "$root" merge-base "$since" HEAD 2>/dev/null) || {
        echo "tools/clang-tidy.sh: no commit that HEAD and '$since' both come from" >&2
        exit 2
    }

    git -C "$root" diff --name-only --diff-filter=d "$base" -- src tests | while IFS= read -r path; do
        case $path in
            *.cpp) printf '%s
' "$root/$path" ;;
            *.h | *.hpp) grep -F "/$(basename "${path%.*}").cpp" "$list" ;;
        esac
    done | sort -u > "$changed"

    picked=$(mktemp)
    grep -F -x -f "$changed" "$list" > "$picked"
    mv "$picked" "$list"

    if [ ! -s "$list" ]; then
        echo "clang-tidy: none of Phazite's files in $build changed since $(git -C "$root" rev-parse --short "$base")"
        exit 0
    fi
fi

count=$(wc -l < "$list" | tr -d ' ')

if [ "$count" -eq 0 ]; then
    echo "tools/clang-tidy.sh: no files of Phazite's in $build/compile_commands.json match '$pattern'" >&2
    exit 2
fi

echo "clang-tidy ($("$tidy" --version | sed -n 's/.*version \([0-9.]*\).*/\1/p' | head -n 1)) over $count files, $jobs at once"

# An MSVC build's database has no Clang options: the one CMakeLists.txt gives Clang for the drum sampler's constexpr names.
set -- -p "$build" --quiet

if grep -q 'cl\.exe' "$build/compile_commands.json"; then
    set -- "$@" --extra-arg=/clang:-fconstexpr-steps=16777216
fi

# One clang-tidy a file; xargs ends 123 when any of them found something (or failed).
if tr '\n' '\0' < "$list" | MSYS_NO_PATHCONV=1 xargs -0 -n 1 -P "$jobs" "$tidy" "$@"; then
    echo "clang-tidy: no findings"
    exit 0
fi

echo "clang-tidy: findings above" >&2
exit 1
