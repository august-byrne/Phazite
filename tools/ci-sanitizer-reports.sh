#!/bin/sh
# For CI: each sanitizer report in a test run's output (the first five), with every frame, as one GitHub annotation.
# The annotations' API serves them without a login, unlike the log, and the problem matchers' annotations of single
# frames stop at ten a step. A report runs from its WARNING or ERROR line to its SUMMARY line; paths lose the runner's
# checkout folder and frames their module offsets, to stay short.
# Usage: sh tools/ci-sanitizer-reports.sh <the tests' output>
set -eu

awk '
    function emit() {
        if (count >= 5 || text == "")
            return

        count++
        gsub(/%/, "%25", text)
        gsub(/\r/, "%0D", text)
        gsub(/\n/, "%0A", text)
        print "::warning title=Sanitizer report " count "::" substr(text, 1, 60000)
    }

    /^(==[0-9]+==)?(WARNING|ERROR): [A-Za-z]+Sanitizer/ {
        inReport = 1
        text = ""
    }

    inReport {
        line = $0
        gsub("/home/runner/work/Phazite/Phazite/", "", line)
        sub(/ \([^ ()]*\+0x[0-9a-f]+\)$/, "", line)
        text = text line "\n"

        if (line ~ /^SUMMARY: /) {
            emit()
            inReport = 0
        }
    }

    END {
        if (inReport)
            emit()
    }
' "$1"
