#!/bin/sh
# Formats Phazite's own C/C++ files with clang-format and the repository's .clang-format (JUCE style).
#   sh tools/format.sh           format every tracked C/C++ file in place
#   sh tools/format.sh --check   only list the files that aren't formatted
#   sh tools/format.sh --staged  the same, for the staged versions (.githooks/pre-commit runs this)
#   sh tools/format.sh --which   print the clang-format it uses
# Exit codes: 0 = done or all formatted, 1 = files need formatting, 2 = clang-format missing or too old.
# It takes clang-format-23, clang-format or clang-format-22 from PATH, then (on Windows) LLVM's install
# folder, then VS Code's C/C++ extension, which bundles one. .clang-format needs version 22 or newer;
# Phazite uses 23.
# On Windows, run it with Git's sh, for example from Git Bash or Claude Code's Bash tool.

cd "$(git rev-parse --show-toplevel)" || exit 2

mode=${1:-}
case "$mode" in
    '' | --check | --staged | --which) ;;
    *)
        echo "usage: sh tools/format.sh [--check | --staged | --which]" >&2
        exit 2 ;;
esac

# Phazite's own C/C++ files. The submodules under external/ aren't tracked here, so they never match.
cxx_files() {
    git -c core.quotepath=off "$@" -- '*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hh' '*.hpp' '*.hxx' '*.inl' '*.ipp' '*.mm'
}

# Nothing C/C++ staged: nothing to check, so clang-format isn't needed.
if [ "$mode" = --staged ] && [ -z "$(cxx_files diff --cached --name-only --diff-filter=ACMR)" ]; then
    exit 0
fi

find_clang_format() {
    # LLVM's apt packages install versioned names next to Ubuntu's older plain clang-format.
    for name in clang-format-23 clang-format clang-format-22; do
        if command -v "$name" >/dev/null 2>&1; then
            command -v "$name"
            return 0
        fi
    done
    case "$(uname -s)" in
        MINGW* | MSYS* | CYGWIN*)
            llvm="$(cygpath -u "${ProgramW6432:-$PROGRAMFILES}")/LLVM/bin/clang-format.exe"
            if [ -x "$llvm" ]; then
                printf '%s\n' "$llvm"
                return 0
            fi
            # The newest C/C++ extension installed in VS Code.
            bundled=$(ls -d "$(cygpath -u "$USERPROFILE")"/.vscode/extensions/ms-vscode.cpptools-*/LLVM/bin/clang-format.exe 2>/dev/null |
                sort -V | tail -n 1)
            if [ -n "$bundled" ] && [ -x "$bundled" ]; then
                printf '%s\n' "$bundled"
                return 0
            fi ;;
    esac
    return 1
}

clang_format=$(find_clang_format) || {
    echo "clang-format wasn't found. Install VS Code's C/C++ extension (tools\\setup-windows.cmd does this), or LLVM" >&2
    echo "(tools\\setup-windows.cmd -WithLlvm). On Linux, install clang-format 23 (see README.md, Code formatting)." >&2
    exit 2
}

# "clang-format version 23.1.0 (...)" or "Ubuntu clang-format version 18.1.3 (...)"
major=$("$clang_format" --version | sed -n 's/.*version \([0-9][0-9]*\)\..*/\1/p' | head -n 1)
if [ -n "$major" ] && [ "$major" -lt 22 ]; then
    echo "$clang_format is version $major; .clang-format needs clang-format 22 or newer (Phazite uses 23)." >&2
    exit 2
fi

case "$mode" in
    '')
        cxx_files ls-files | while IFS= read -r file; do
            "$clang_format" --style=file -i "$file" || exit 2
        done ;;
    --check)
        unformatted=$(cxx_files ls-files | while IFS= read -r file; do
            "$clang_format" --style=file --dry-run --Werror "$file" >/dev/null 2>&1 || printf '    %s\n' "$file"
        done)
        [ -z "$unformatted" ] || { printf '%s\n' "$unformatted"; exit 1; } ;;
    --staged)
        unformatted=$(cxx_files diff --cached --name-only --diff-filter=ACMR | while IFS= read -r file; do
            git show ":$file" | "$clang_format" --style=file --assume-filename="$file" --dry-run --Werror >/dev/null 2>&1 ||
                printf '    %s\n' "$file"
        done)
        [ -z "$unformatted" ] || { printf '%s\n' "$unformatted"; exit 1; } ;;
    --which)
        printf '%s\n' "$clang_format"
        "$clang_format" --version ;;
esac
