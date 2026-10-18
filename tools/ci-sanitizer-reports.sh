#!/bin/sh
# For CI: each sanitizer report in a test run's output (the first five) as one GitHub annotation. The annotations' API
# serves them without a login, unlike the log, and the problem matchers' annotations of single frames stop at ten a step.
# A report runs from its WARNING or ERROR line to its SUMMARY line. GitHub keeps an annotation's first 4096 characters
# only, so paths lose the runner's checkout folder (and Tracktion's and JUCE's, their modules folder), frames their module
# offsets and build IDs, and each stack its frames after the first ten (the message loop and std::function's insides, as a
# rule), so that both stacks of a race fit.
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
        print "::warning title=Sanitizer report " count "::" substr(text, 1, 4096)
    }

    /^(==[0-9]+==)?(WARNING|ERROR): [A-Za-z]+Sanitizer/ {
        inReport = 1
        text = ""
    }

    inReport {
        line = $0
        gsub("/home/runner/work/Phazite/Phazite/", "", line)
        gsub("external/tracktion_engine/modules/", "", line)
        sub(/ \(BuildId: [0-9a-f]+\)$/, "", line)
        sub(/ \([^ ()]*\+0x[0-9a-f]+\)$/, "", line)

        # A frame: "    #10 function file:line".
        if (line ~ /^ +#[0-9]+ /) {
            frame = line
            sub(/^ +#/, "", frame)

            if (frame + 0 > 10)
                next

            if (frame + 0 == 10) {
                text = text "    ...\n"
                next
            }
        }

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
