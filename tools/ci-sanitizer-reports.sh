#!/bin/sh
# For CI: each sanitizer report in a test run's output (the first five annotations) as GitHub annotations. The
# annotations' API serves them without a login, unlike the log, and the problem matchers' annotations of single frames stop
# at ten a step. A report runs from its WARNING or ERROR line to its SUMMARY line. GitHub keeps an annotation's first 4096
# characters only, so paths lose the runner's checkout folder (and Tracktion's and JUCE's, their modules folder), frames
# their module offsets and build IDs, and each stack its frames after the first ten (the message loop and std::function's
# insides, as a rule), so that both stacks of a race fit.
#
# A leak report lists every allocation still held at exit, each from inside JUCE's containers and the standard library,
# where the first ten frames say nothing of who made it: its leaks are grouped instead by the first five frames outside
# them (the code that made what leaked), with how many leaks and bytes each group has, direct leaks first (a cycle of
# references leaks indirectly only), over as many annotations as they take.
# Usage: sh tools/ci-sanitizer-reports.sh <the tests' output>
set -eu

awk '
    function emit(body) {
        if (count >= 5 || body == "")
            return

        count++
        gsub(/%/, "%25", body)
        gsub(/\r/, "%0D", body)
        gsub(/\n/, "%0A", body)
        print "::warning title=Sanitizer report " count "::" substr(body, 1, 4096)
    }

    # A leak group ends: counted under its frames.
    function endGroup() {
        if (! inGroup)
            return

        inGroup = 0
        key = kind "\n" frames

        if (! (key in leaks)) {
            keys[++numKeys] = key
            leaks[key] = 0
            bytes[key] = 0
        }

        leaks[key]++
        bytes[key] += groupBytes
    }

    # The groups, direct leaks first, as many annotations as they fill.
    function emitLeaks(   pass, i, wanted, body, entry, split_) {
        body = header

        for (pass = 1; pass <= 2; pass++) {
            wanted = pass == 1 ? "Direct" : "Indirect"

            for (i = 1; i <= numKeys; i++) {
                split(keys[i], split_, "\n")

                if (split_[1] != wanted)
                    continue

                entry = leaks[keys[i]] " " tolower(wanted) " leak(s) group, " bytes[keys[i]] " bytes, made by:\n" substr(keys[i], length(wanted) + 2) "\n"

                if (length(body) + length(entry) > 4000) {
                    emit(body)
                    body = header
                }

                body = body entry
            }
        }

        emit(body summary)
        split("", leaks)
        split("", bytes)
        split("", keys)
        numKeys = 0
    }

    /^(==[0-9]+==)?(WARNING|ERROR): [A-Za-z]+Sanitizer/ {
        inReport = 1
        isLeak = $0 ~ /LeakSanitizer/
        text = ""
        header = $0 "\n"
        inGroup = 0
    }

    inReport {
        line = $0
        gsub("/home/runner/work/Phazite/Phazite/", "", line)
        gsub("external/tracktion_engine/modules/", "", line)
        sub(/ \(BuildId: [0-9a-f]+\)$/, "", line)
        sub(/ \([^ ()]*\+0x[0-9a-f]+\)$/, "", line)

        if (isLeak) {
            if (line ~ /^(Direct|Indirect) leak of [0-9]+ byte/) {
                endGroup()
                inGroup = 1
                kind = line ~ /^Direct/ ? "Direct" : "Indirect"
                groupBytes = line
                sub(/^(Direct|Indirect) leak of /, "", groupBytes)
                sub(/ byte.*/, "", groupBytes)
                groupBytes += 0
                frames = ""
                shown = 0
            } else if (inGroup && line ~ /^ +#[0-9]+ /) {
                # Inside the allocator, JUCE'"'"'s containers, strings and values, and the standard library: what every
                # allocation goes through.
                if (line ~ /libsanitizer|juce_core\/(memory|containers|text|maths)\/|juce_data_structures\/values\// \
                    || line ~ / in (std::|__gnu_cxx|operator new|malloc|calloc|realloc)/ || line ~ /\/(bits|ext)\//)
                    next

                if (shown < 5) {
                    sub(/^ +#[0-9]+ 0x[0-9a-f]+ in /, "    ", line)
                    frames = frames substr(line, 1, 220) "\n"
                    shown++
                }
            } else if (line ~ /^SUMMARY: /) {
                endGroup()
                summary = line "\n"
                emitLeaks()
                inReport = 0
            } else if (line == "") {
                endGroup()
            }

            next
        }

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
            emit(text)
            inReport = 0
        }
    }

    END {
        if (inReport && isLeak) {
            endGroup()
            emitLeaks()
        } else if (inReport) {
            emit(text)
        }
    }
' "$1"
