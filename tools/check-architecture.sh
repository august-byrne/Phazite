#!/bin/sh
# Checks that Phazite's code keeps to its architecture rules (CLAUDE.md, "Architecture rules"; docs/design.md,
# section 24). It counts what the rules forbid in src/, plus includes and CMake source paths that Linux wouldn't
# find, and compares each count with its budget in tools/architecture-budget.txt. Every count must equal its
# budget, and budgets only go down: a commit that removes matches lowers the budget (--update), and a commit
# that raises one on purpose says why in its message, with a line
#     Architecture-Budget-Raise: <rule> <old> -> <new>: <reason>
#
#   sh tools/check-architecture.sh [--base REV]  check the working tree: print each count with its budget and,
#                                                for a count over its budget, the files whose count grew since
#                                                REV (default HEAD)
#   sh tools/check-architecture.sh --staged      check the staged code against the staged budgets, and that no
#                                                budget is above HEAD's (in a merge: HEAD's or MERGE_HEAD's);
#                                                prints problems only (.githooks/pre-commit runs this)
#   sh tools/check-architecture.sh --update      lower each budget whose count is lower (it never raises one)
#   sh tools/check-architecture.sh --list RULE   list every match of one rule in the working tree
#   sh tools/check-architecture.sh --range ARGS  check that every commit of "git rev-list ARGS" (one argument,
#                                                such as "main..HEAD") that raises a budget has its
#                                                Architecture-Budget-Raise line (.githooks/pre-push runs this)
#   sh tools/check-architecture.sh --ci          what CI's Checks job runs: the check compared with the pull
#                                                request's base or the commit before the push (the first of
#                                                $PHAZITE_CI_BASES that is an ancestor of HEAD, else where the
#                                                branch left origin/main), then --range over the commits since
#   sh tools/check-architecture.sh --self-test   check the counting and the budget logic on built-in examples
#                                                (every other mode does this first)
# The rules (the layers: src/app uses src/ui, src/ui uses src/services and src/model, src/services uses
# src/model):
#   upward-includes           includes from model or services into ui or app, from model into services, or
#                             from ui into app
#   model-window-includes     JUCE window modules (juce_gui_basics, juce_gui_extra, juce_opengl, juce_video)
#                             included in model or services
#   model-window-references   JUCE window classes (juce::Component, juce::KeyPress...) named in model or
#                             services
#   model-tracktion-includes  Tracktion included in model or services outside the prelude
#   model-undo-steps          beginNewTransaction in model or services outside the undo code
#   model-include-loops       model and services files whose includes lead back to themselves
#   ui-tracktion-includes     Tracktion, or the model's prelude, included in ui
#   ui-tracktion-references   te:: and tracktion:: names in ui
#   ui-tracktion-aliases      using-declarations, aliases and typedefs in ui that would hide Tracktion names
#   ui-undo-steps             calls that begin an undo step, in ui and app
#   flat-files                C/C++ files directly in src/model or src/ui instead of an area's folder
#   unresolved-includes       #include "..." lines in src/, tests/ and tools/ that name no file with its exact
#                             case (next to the including file, else from src/)
#   cmake-sources             source paths in CMakeLists.txt that name no file exactly, and C/C++ files in src/
#                             or tests/ that it doesn't list
# Comments and the text of string and character literals don't count. A C/C++ file in src/ outside the four
# layers fails the check whatever the budgets.
# Exit codes: 0 = every count equals its budget (and every raise checked says why); 1 = a count differs from
# its budget, or a file is in no layer; 2 = it couldn't run; 3 = the counts match, but a budget went up
# (--staged: above HEAD's; --range and --ci: without its Architecture-Budget-Raise line).
# It needs git and a POSIX sh and awk ($AWK if set; CI runs it with dash and mawk). On Windows, run it with
# Git's sh, for example from Git Bash or Claude Code's Bash tool.

cd "$(git rev-parse --show-toplevel)" || exit 2

# Settings. Lists of paths are separated by spaces.
# The model's prelude headers: the only model files that may include Tracktion, and model headers that src/ui
# may not include.
tracktion_preludes='src/model/base/Tracktion.h'
# The calls that begin an undo step (an extended regular expression), counted in src/ui and src/app.
undo_functions='beginNewTransaction'
# The undo code: the model files that may call beginNewTransaction.
undo_implementation=''

budget_file=tools/architecture-budget.txt
awk=${AWK:-awk}
gha=0
[ "${GITHUB_ACTIONS:-}" = true ] && gha=1

usage() {
    echo "usage: sh tools/check-architecture.sh [--base REV | --staged | --update | --list RULE |" >&2
    echo "       --range \"REV-LIST ARGS\" | --ci | --self-test | --help]" >&2
    exit 2
}

mode=check base= rule= range=
case ${1:-} in
    '') [ $# -eq 0 ] || usage ;;
    --base) [ $# -eq 2 ] || usage; base=$2 ;;
    --staged | --update | --ci | --self-test | --help) [ $# -eq 1 ] || usage; mode=${1#--} ;;
    --list) [ $# -eq 2 ] || usage; mode=list rule=$2 ;;
    --range) [ $# -eq 2 ] || usage; mode=range range=$2 ;;
    *) usage ;;
esac

# The counting and the budget logic. It reads sections, each starting with a line "@name [argument]"; the lines
# of a file's contents start with "=", so that no line of a file can start a section.
#   @absent               paths that @files lists but the code doesn't have (this section comes first)
#   @files                C/C++ and other paths under src/, tests/ and tools/, one per line
#   @tree                 every line of those files as git grep prints it: [revision:]path:line:text
#   @status N             git grep's exit code
#   @cmake                CMakeLists.txt
#   @budgets              the budget file that is checked
#   @head-budgets NAME    (--staged) HEAD's budget file, and MERGE_HEAD's in a merge
#   @base-budgets NAME    the budget file of the revision compared with
#   @counts, @base-counts what mode count printed for the code, and for the revision compared with
#   @commit, @commit-budgets, @parent-budgets   (--range) a commit ("hash subject", then its message), its
#                         budget file, and each parent's
#   @expect RULE N        (self-test) what the built-in examples must count
# Modes: count, list, check, staged, update, range and self-test. The program holds no escape sequences, which
# awks treat differently: BS, DQ and CR are a backslash, a double quote and a carriage return, and q (set on the
# command line) is a single quote.
awk_program() {
    cat <<'AWK'
BEGIN {
    setup()
    if (mode == "list" && !(listing in known)) {
        fatal("There is no rule called " listing ". The rules: " ruleList() ".")
        exit
    }
}

function setup(    i, n, s, parts) {
    BS = sprintf("%c", 92)
    DQ = sprintf("%c", 34)
    CR = sprintf("%c", 13)
    special = "//|/[*]|R" DQ "|" DQ "|" q
    W = "[^A-Za-z0-9_]"
    CXX = "[.](c|cc|cpp|cxx|h|hh|hpp|hxx|inl|ipp|mm)$"
    BUDGETS = "tools/architecture-budget.txt"

    ruleNames = "upward-includes model-window-includes model-window-references model-tracktion-includes"
    ruleNames = ruleNames " model-undo-steps model-include-loops ui-tracktion-includes ui-tracktion-references"
    ruleNames = ruleNames " ui-tracktion-aliases ui-undo-steps flat-files unresolved-includes cmake-sources"
    nrules = split(ruleNames, rules, " ")
    for (i = 1; i <= nrules; i++) known[rules[i]] = 1

    s = "Component|ComponentPeer|ComponentListener|Desktop|Displays|TopLevelWindow|DocumentWindow"
    s = s "|ResizableWindow|DialogWindow|AlertWindow|NativeMessageBox|MessageBoxOptions|ScopedMessageBox"
    s = s "|FileChooser|PopupMenu|MenuBarModel|KeyPress|KeyPressMappingSet|KeyListener|ModifierKeys"
    s = s "|MouseEvent|MouseListener|MouseCursor|MouseInputSource|FocusChangeListener"
    s = s "|ApplicationCommandManager|ApplicationCommandManagerListener|ApplicationCommandTarget"
    s = s "|ApplicationCommandInfo|CommandID|StandardApplicationCommandIDs|JUCEApplication|LookAndFeel"
    s = s "|LookAndFeel_V4|Button|TextButton|ToggleButton|DrawableButton|Slider|Label|TextEditor|ComboBox"
    s = s "|ListBox|TreeView|TreeViewItem|Viewport|ScrollBar|DragAndDropContainer|DragAndDropTarget"
    s = s "|FileDragAndDropTarget|TextDragAndDropTarget|TooltipWindow|TooltipClient|SettableTooltipClient"
    s = s "|CallOutBox|Drawable|RecentlyOpenedFilesList|KeyMappingEditorComponent|CodeEditorComponent"
    s = s "|ColourSelector|SystemTrayIconComponent|WebBrowserComponent"
    reWindowClass = W "juce::(" s ")" W
    reWindowModule = "^(juce_gui_basics|juce_gui_extra|juce_opengl|juce_video)/"
    reTracktionModule = "^(tracktion_engine|tracktion_graph|tracktion_core)/"
    reInclude = "^[[:blank:]]*#[[:blank:]]*(include|import)[[:blank:]]*[<" DQ "]"
    reModelUndo = W "beginNewTransaction" W
    reUiUndo = W "(" undo_functions ")" W
    reTracktionName = W "(te|tracktion|tracktion_engine|tracktion_graph|tracktion_core)::"
    tn = "(te|tracktion[a-z_]*)"
    reAlias = W "(using[[:blank:]]+namespace[[:blank:]]+(::)?" tn W
    reAlias = reAlias "|using[[:blank:]]+(::)?" tn "::"
    reAlias = reAlias "|using[[:blank:]]+[A-Za-z_][A-Za-z0-9_]*[[:blank:]]*=[^;]*" W tn "::"
    reAlias = reAlias "|typedef[^;]*" W tn "::)"
    reNamespaceAlias = W "namespace[[:blank:]]+[A-Za-z_][A-Za-z0-9_]*[[:blank:]]*=[[:blank:]]*(::)?tracktion"

    n = split(preludes, parts, " ")
    for (i = 1; i <= n; i++) isPrelude[parts[i]] = 1
    n = split(undo_implementation, parts, " ")
    for (i = 1; i <= n; i++) isUndoCode[parts[i]] = 1
    # Arrays that are only passed to functions, declared as arrays for every awk.
    split("", headMax)
    split("", baseFrom)

    # What each rule counts, and what to do instead.
    what["upward-includes"] = "includes from model or services into ui or app, from model into services, or from ui into app"
    instead["upward-includes"] = "dependencies point down (app -> ui -> services -> model): move what both need down a layer, or have the upper layer pass it in."
    what["model-window-includes"] = "JUCE window modules or classes in src/model or src/services (Tracktion" q "s headers bring them in without an include)"
    instead["model-window-includes"] = "move window and key code to src/ui or src/app; the model uses juce_core, juce_events, juce_data_structures, juce_audio_basics, and juce_graphics for colours."
    what["model-window-references"] = what["model-window-includes"]
    instead["model-window-references"] = instead["model-window-includes"]
    what["model-tracktion-includes"] = "Tracktion includes in model or services outside the prelude"
    instead["model-tracktion-includes"] = "include the prelude or the model header you need."
    what["model-undo-steps"] = "beginNewTransaction in model or services outside the undo code"
    instead["model-undo-steps"] = "a command" q "s undoTarget, or phazite::UndoStep in a model function that is a change of its own."
    what["model-include-loops"] = "model and services files whose includes lead back to themselves"
    instead["model-include-loops"] = "move what both need into a lower header, or split the file."
    what["ui-tracktion-includes"] = "Tracktion or the prelude included in src/ui"
    instead["ui-tracktion-includes"] = "include the model header that offers what the UI needs."
    what["ui-tracktion-references"] = "te:: and tracktion:: in src/ui"
    instead["ui-tracktion-references"] = "call a model function, pass IDs and plain values."
    what["ui-tracktion-aliases"] = "aliases that would hide Tracktion names"
    instead["ui-tracktion-aliases"] = "write them out with te::."
    what["ui-undo-steps"] = "calls starting an undo step in src/ui and src/app"
    instead["ui-undo-steps"] = "a command with an undoTarget, or a model function that starts its own step."
    what["flat-files"] = "C/C++ files directly in src/model or src/ui"
    instead["flat-files"] = "put it in its area" q "s folder."
    what["unresolved-includes"] = "#include " DQ "..." DQ " in src/, tests/ or tools/ naming no file exactly"
    instead["unresolved-includes"] = "give the path from src/ (or a same-folder bare name) with its exact case."
    what["cmake-sources"] = "CMakeLists.txt paths naming no file exactly, and C/C++ files under src/ or tests/ it doesn" q "t list"
    instead["cmake-sources"] = "list the file in its target" q "s target_sources, spelled as git ls-files prints it."
}

{ sub(CR "$", "") }
/^@/ { startSection(); next }
section == "absent" { if ($0 != "") absent[$0] = 1; next }
section == "files" { if ($0 != "") addFile($0); next }
section == "tree" { if ($0 != "") treeLine($0); next }
section == "counts" || section == "base-counts" { if ($0 != "") takeCount($0); next }
substr($0, 1, 1) == "=" { takeLine(substr($0, 2)) }

END {
    if (mode == "range") finishCommit()
    if (nfatal) exit failed()
    if (mode == "count" || mode == "list" || mode == "self-test") {
        countFiles()
        countCmake()
        countLoops()
    }
    if (mode == "count") {
        dump()
        exit 0
    }
    if (mode == "list") {
        say("Total: " (total[listing] + 0))
        exit 0
    }
    if (mode == "self-test") exit selfTest()
    if (mode == "range") exit rangeResult()
    exit report()
}

# ---- Input ----

function startSection() {
    section = substr($1, 2)
    sectionArg = $2
    lineNo = 0
    if (section == "status" && $2 + 0 >= 2) fatal("git grep failed (exit code " $2 "), so nothing was counted.")
    else if (section == "expect") {
        expect[$2] = $3 + 0
        expectOrder[++nexpect] = $2
    }
    else if (section == "base-counts") haveBaseCounts = 1
    else if (section == "commit") {
        finishCommit()
        inCommit = 1
        nmessage = 0
        ncommitRules = 0
        split("", message)
        split("", commitBud)
        split("", commitRules)
        split("", parentMax)
        split("", parentFrom)
    }
}

function takeLine(line) {
    lineNo++
    if (section == "cmake") cmake[++ncmake] = line
    else if (section == "budgets") takeBudget(line)
    else if (section == "head-budgets") {
        if (parseBudget(line) == 1) {
            keepMax(headMax, headFrom, bRule, bValue, sectionArg)
            if (sectionArg == "HEAD") keepMax(baseBud, baseFrom, bRule, bValue, sectionArg)
        }
    }
    else if (section == "base-budgets") {
        if (parseBudget(line) == 1) keepMax(baseBud, baseFrom, bRule, bValue, sectionArg)
    }
    else if (section == "commit") {
        if (lineNo == 1) {
            commitHash = line
            sub("[[:blank:]].*$", "", commitHash)
            commitSubject = substr(line, length(commitHash) + 2)
        }
        else message[++nmessage] = line
    }
    else if (section == "commit-budgets") {
        if (parseBudget(line) == 1 && !(bRule in commitBud)) {
            commitBud[bRule] = bValue
            commitRules[++ncommitRules] = bRule
        }
    }
    else if (section == "parent-budgets") {
        if (parseBudget(line) == 1) keepMax(parentMax, parentFrom, bRule, bValue, "")
    }
}

# A line of the budget file that is checked: every rule must have exactly one budget.
function takeBudget(line,    kind) {
    budgetLine[++nbudgetLines] = line
    kind = parseBudget(line)
    if (kind == 0) return
    if (kind < 0) fatal(BUDGETS ":" lineNo ": " DQ trim(line) DQ " isn" q "t " DQ "<rule> <number>" DQ ".")
    else if (!(bRule in known)) fatal(BUDGETS ":" lineNo ": names an unknown rule: " bRule ". The rules: " ruleList() ".")
    else if (bRule in bud) fatal(BUDGETS ":" lineNo ": " bRule " has a budget already, on line " budAt[bRule] ".")
    else {
        bud[bRule] = bValue
        budAt[bRule] = lineNo
    }
}

# A line that mode count printed: "total RULE N", "key RULE N KEY" or "nolayer PATH".
function takeCount(line,    f, key) {
    split(line, f, " ")
    if (f[1] == "total") {
        if (section == "counts") cnt[f[2]] = f[3] + 0
        else baseCnt[f[2]] = f[3] + 0
    }
    else if (f[1] == "key") {
        key = substr(line, length(f[1]) + length(f[2]) + length(f[3]) + 4)
        if (section == "counts") keyCnt[f[2], key] = f[3] + 0
        else baseKeyCnt[f[2], key] = f[3] + 0
    }
    else if (f[1] == "nolayer" && section == "counts") noLayer[++nnolayer] = substr(line, 9)
}

function addFile(path) {
    if ((path in isFile) || (path in absent)) return
    isFile[path] = 1
    files[++nfiles] = path
}

# ---- Counting ----

function layerOf(path) {
    if (index(path, "src/model/") == 1) return "model"
    if (index(path, "src/services/") == 1) return "services"
    if (index(path, "src/ui/") == 1) return "ui"
    if (index(path, "src/app/") == 1) return "app"
    return ""
}

function treeLine(line,    path, lineno, text, s) {
    if (prefix != "" && index(line, prefix ":") == 1) line = substr(line, length(prefix) + 2)
    if (!match(line, ":[0-9]+:")) return
    path = substr(line, 1, RSTART - 1)
    lineno = substr(line, RSTART + 1, RLENGTH - 2)
    text = substr(line, RSTART + RLENGTH)
    if (path != curPath) {
        curPath = path
        inblock = 0
        rawend = ""
        curLayer = layerOf(path)
        curCxx = (path ~ CXX)
    }
    if (!curCxx) return
    s = strip(text)
    if (s ~ reInclude) takeInclude(path, lineno, text, s)
    if (curLayer == "model" || curLayer == "services") {
        if (index(s, "juce::")) hitLine("model-window-references", path, lineno, text, count(s, reWindowClass))
        if (index(s, "beginNewTransaction") && !(path in isUndoCode))
            hitLine("model-undo-steps", path, lineno, text, count(s, reModelUndo))
    }
    else if (curLayer == "ui") {
        if (index(s, "::")) hitLine("ui-tracktion-references", path, lineno, text, count(s, reTracktionName))
        if (s ~ /using|typedef|namespace/)
            hitLine("ui-tracktion-aliases", path, lineno, text, count(s, reAlias) + namespaceAliases(s))
        hitLine("ui-undo-steps", path, lineno, text, count(s, reUiUndo))
    }
    else if (curLayer == "app") hitLine("ui-undo-steps", path, lineno, text, count(s, reUiUndo))
}

function takeInclude(path, lineno, text, s,    delim, rest, k, target, res) {
    match(s, reInclude)
    delim = substr(s, RSTART + RLENGTH - 1, 1)
    rest = substr(s, RSTART + RLENGTH)
    k = index(rest, delim == "<" ? ">" : DQ)
    if (k <= 1) return
    target = substr(rest, 1, k - 1)
    res = resolve(path, target, delim)
    if (curLayer == "model" || curLayer == "services") {
        if (target ~ reWindowModule) hitLine("model-window-includes", path, lineno, text, 1)
        if (target ~ reTracktionModule && !(path in isPrelude)) hitLine("model-tracktion-includes", path, lineno, text, 1)
        if (index(res, "src/ui/") == 1 || index(res, "src/app/") == 1 || (curLayer == "model" && index(res, "src/services/") == 1))
            hitLine("upward-includes", path, lineno, text, 1)
        if (delim == DQ && (index(res, "src/model/") == 1 || index(res, "src/services/") == 1))
            addEdge(component(path), component(res))
    }
    else if (curLayer == "ui") {
        if (target ~ reTracktionModule || (res in isPrelude)) hitLine("ui-tracktion-includes", path, lineno, text, 1)
        if (index(res, "src/app/") == 1) hitLine("upward-includes", path, lineno, text, 1)
    }
    if (delim == DQ && !(res in isFile)) hitLine("unresolved-includes", path, lineno, text, 1)
}

function hit(rule, key, n) {
    total[rule] += n
    keyCount[rule, key] += n
}

function hitLine(rule, path, lineno, text, n) {
    if (n <= 0) return
    hit(rule, path, n)
    if (rule == listing) say(path ":" lineno ": " trim(text) (n > 1 ? "  (" n ")" : ""))
}

# How many times re matches s, counting matches that share a delimiter.
function count(s, re,    n) {
    n = 0; s = " " s " "
    while (match(s, re)) { n++; s = substr(s, RSTART + RLENGTH - 1) }
    return n
}

# Aliases such as "namespace tr = tracktion;": every name but te.
function namespaceAliases(s,    n, m, name) {
    n = 0; s = " " s " "
    while (match(s, reNamespaceAlias)) {
        m = substr(s, RSTART + 1, RLENGTH - 1)
        s = substr(s, RSTART + RLENGTH)
        sub("^namespace[[:blank:]]+", "", m)
        name = m
        sub("[^A-Za-z0-9_].*$", "", name)
        if (name != "te") n++
    }
    return n
}

# Whether out ends with an identifier or a number (not with a literal prefix such as u8 or L).
function identEnd(out) { return out ~ /[A-Za-z0-9_]$/ && !(out ~ /^(u8|u|U|L)$/ || out ~ /[^A-Za-z0-9_](u8|u|U|L)$/) }

# The code of one line: comments removed (also across lines), and string, character and raw string literals
# emptied (also across lines), except the path of an #include or #import.
function strip(s,    out, keep, k, c, tok, delim, lit, j) {
    out = ""; keep = (s ~ /^[[:blank:]]*#[[:blank:]]*(include|import)/)
    while (s != "") {
        if (inblock) { k = index(s, "*/"); if (k == 0) return out
                       s = substr(s, k + 2); inblock = 0; out = out " "; continue }
        if (rawend != "") { k = index(s, rawend); if (k == 0) return out
                            s = substr(s, k + length(rawend)); rawend = ""; out = out DQ DQ; continue }
        if (!match(s, special)) return out s
        out = out substr(s, 1, RSTART - 1); tok = substr(s, RSTART, RLENGTH); s = substr(s, RSTART + RLENGTH)
        if (tok == "//") return out
        if (tok == "/*") { inblock = 1; continue }
        if (tok == "R" DQ) {
            if (identEnd(out)) { out = out "R"; s = DQ s; continue }
            k = index(s, "("); delim = (k > 1) ? substr(s, 1, k - 1) : ""
            rawend = ")" delim DQ; s = substr(s, k + 1); continue
        }
        if (tok == q && identEnd(out)) { out = out q; continue }
        lit = tok; j = 1
        while (j <= length(s)) { c = substr(s, j, 1)
            if (c == BS) { lit = lit substr(s, j, 2); j += 2; continue }
            lit = lit c; j++; if (c == tok) break }
        s = substr(s, j); out = out (keep ? lit : tok tok)
    }
    return out
}

function normalise(path,    parts, n, i, stack, top, res) {
    n = split(path, parts, "/"); top = 0
    for (i = 1; i <= n; i++) {
        if (parts[i] == "." || parts[i] == "") continue
        if (parts[i] == "..") { if (top > 0) top--; continue }
        stack[++top] = parts[i]
    }
    res = ""; for (i = 1; i <= top; i++) res = res (i > 1 ? "/" : "") stack[i]
    return res
}

function dirname(file,    d) { d = file; sub("/[^/]*$", "", d); return d }

# Where an include points: a quoted one next to the including file if it starts with ./ or ../ or a file is
# there, else (and every <...> one) under src/, the include path of Phazite.
function resolve(file, target, delim,    near) {
    if (delim == DQ) {
        near = normalise(dirname(file) "/" target)
        if (index(target, "./") == 1 || index(target, "../") == 1 || (near in isFile)) return near
    }
    return normalise("src/" target)
}

function component(path) { sub("[.][^./]*$", "", path); return path }

function addEdge(from, to) {
    if (from == to || ((from, to) in edge)) return
    edge[from, to] = 1
    adjacent[from] = adjacent[from] " " to
}

# Whether an include path leads from node back to target (depth first; visited is cleared by the caller).
function reaches(node, target,    list, n, i, nxt) {
    n = split(adjacent[node], list, " ")
    for (i = 1; i <= n; i++) {
        nxt = list[i]
        if (nxt == target) return 1
        if (nxt in visited) continue
        visited[nxt] = 1
        if (reaches(nxt, target)) return 1
    }
    return 0
}

function countLoops(    c, list, n, i) {
    split("", list)
    n = 0
    for (c in adjacent) list[++n] = c
    sortList(list, n)
    for (i = 1; i <= n; i++) {
        split("", visited)
        if (!reaches(list[i], list[i])) continue
        hit("model-include-loops", list[i], 1)
        if (listing == "model-include-loops") say(list[i])
    }
}

# Files in no layer, and files directly in src/model or src/ui.
function countFiles(    i, f, rest) {
    for (i = 1; i <= nfiles; i++) {
        f = files[i]
        if (f !~ CXX || index(f, "src/") != 1) continue
        if (layerOf(f) == "") noLayer[++nnolayer] = f
        rest = ""
        if (index(f, "src/model/") == 1) rest = substr(f, 11)
        else if (index(f, "src/ui/") == 1) rest = substr(f, 8)
        if (rest == "" || index(rest, "/") > 0) continue
        hit("flat-files", f, 1)
        if (listing == "flat-files") say(f)
    }
}

# A CMake line without its comment (from a # outside double quotes).
function cmakeCode(line,    i, c, inquotes, out) {
    if (index(line, "#") == 0) return line
    inquotes = 0; out = ""
    for (i = 1; i <= length(line); i++) {
        c = substr(line, i, 1)
        if (c == "#" && !inquotes) break
        if (c == BS && inquotes) { out = out substr(line, i, 2); i++; continue }
        if (c == DQ) inquotes = !inquotes
        out = out c
    }
    return out
}

# Source paths in CMakeLists.txt that name no file, and C/C++ files in src/ and tests/ that it does not name.
function countCmake(    i, j, n, line, words, w, f, named) {
    split("", named)
    for (i = 1; i <= ncmake; i++) {
        line = cmakeCode(cmake[i])
        gsub("[()]", " ", line)
        n = split(line, words, " ")
        for (j = 1; j <= n; j++) {
            w = words[j]
            if (length(w) >= 2 && substr(w, 1, 1) == DQ && substr(w, length(w)) == DQ) w = substr(w, 2, length(w) - 2)
            if (w !~ "^(src|tests|tools)/" || w !~ CXX) continue
            named[w] = 1
            if (w in isFile) continue
            hit("cmake-sources", w, 1)
            if (listing == "cmake-sources") say("CMakeLists.txt:" i ": " w " names no file")
        }
    }
    for (i = 1; i <= nfiles; i++) {
        f = files[i]
        if (f !~ CXX || (index(f, "src/") != 1 && index(f, "tests/") != 1) || (f in named)) continue
        hit("cmake-sources", f, 1)
        if (listing == "cmake-sources") say(f ": CMakeLists.txt doesn" q "t list it")
    }
}

function dump(    i, k, parts) {
    for (i = 1; i <= nrules; i++) print "total", rules[i], total[rules[i]] + 0
    for (k in keyCount) {
        split(k, parts, SUBSEP)
        print "key", parts[1], keyCount[k], parts[2]
    }
    for (i = 1; i <= nnolayer; i++) print "nolayer", noLayer[i]
}

# ---- Budgets ----

# Reads a budget line: returns 1 and sets bRule and bValue for "<rule> <number>", 0 for a blank or comment
# line, and -1 for anything else.
function parseBudget(line,    f) {
    sub(CR "$", "", line)
    if (line ~ /^[[:blank:]]*$/ || line ~ /^[[:blank:]]*#/) return 0
    if (line !~ /^[[:blank:]]*[^[:blank:]#]+[[:blank:]]+[0-9]+[[:blank:]]*$/) return -1
    split(line, f, " ")
    bRule = f[1]
    bValue = f[2] + 0
    return 1
}

# Keeps the highest budget each rule has in the files read so far, and the name of the file it came from.
function keepMax(budgets, from, r, v, name) {
    if (!(r in budgets) || v + 0 > budgets[r] + 0) {
        budgets[r] = v + 0
        from[r] = name
    }
}

# Compares counts with budgets; fills over and under with the differences and returns how many rules differ.
function judge(names, n, counts, budgets, over, under,    i, r, d) {
    d = 0
    for (i = 1; i <= n; i++) {
        r = names[i]
        if (counts[r] + 0 > budgets[r] + 0) { over[r] = counts[r] - budgets[r]; d++ }
        else if (counts[r] + 0 < budgets[r] + 0) { under[r] = budgets[r] - counts[r]; d++ }
    }
    return d
}

# The rules whose budget is above the one in before (a rule that before lacks is not raised).
function raises(names, n, now, before, raised,    i, r, k) {
    k = 0
    for (i = 1; i <= n; i++) {
        r = names[i]
        if ((r in now) && (r in before) && now[r] + 0 > before[r] + 0) { raised[r] = before[r]; k++ }
    }
    return k
}

function exitStatus(ndiff, nraise, nolayer) { return (ndiff > 0 || nolayer > 0) ? 1 : (nraise > 0 ? 3 : 0) }

# A budget line with value in place of its number, right-aligned where the old number ended.
function withValue(line, value,    head, rest, numberEnd, out) {
    sub(CR "$", "", line)
    match(line, "^[[:blank:]]*[^[:blank:]]+")
    head = substr(line, 1, RLENGTH)
    rest = substr(line, RLENGTH + 1)
    match(rest, "[0-9]+")
    numberEnd = length(head) + RSTART + RLENGTH - 1
    out = head " "
    value = value ""
    while (length(out) + length(value) < numberEnd) out = out " "
    return out value
}

# Lowers each budget line whose count is lower; returns how many, with the old values in lowered.
function lower(lines, n, counts, lowered,    i, k) {
    k = 0
    for (i = 1; i <= n; i++) {
        if (parseBudget(lines[i]) != 1 || !(bRule in counts) || counts[bRule] + 0 >= bValue) continue
        lowered[bRule] = bValue
        lines[i] = withValue(lines[i], counts[bRule] + 0)
        k++
    }
    return k
}

# Whether a commit message explains a raise of rule.
function explained(lines, n, rule,    i, start) {
    start = "^Architecture-Budget-Raise:[[:blank:]]*" rule
    for (i = 1; i <= n; i++)
        if (lines[i] ~ (start "$") || lines[i] ~ (start "[[:blank:]:]")) return 1
    return 0
}

# ---- Reports ----

function say(s) { print s; said = 1 }
function blank() { if (said) print "" }
function fatal(s) { fatalMessage[++nfatal] = s }
function failed(    i) { for (i = 1; i <= nfatal; i++) print fatalMessage[i]; return 2 }
function trim(s) { sub("^[[:blank:]]+", "", s); sub("[[:blank:]]+$", "", s); return s }
function padTo(s, width) { while (length(s) < width) s = s " "; return s }

function ruleList(    i, s) {
    s = rules[1]
    for (i = 2; i <= nrules; i++) s = s ", " rules[i]
    return s
}

function sortList(a, n,    i, j, t) {
    for (i = 2; i <= n; i++) {
        t = a[i]
        for (j = i - 1; j > 0 && a[j] > t; j--) a[j + 1] = a[j]
        a[j + 1] = t
    }
}

# Modes check, update and staged.
function report(    i, r, ndiff, nover, nraise, nlow, over, under, raised, lowered) {
    for (i = 1; i <= nrules; i++)
        if (!(rules[i] in bud)) fatal(BUDGETS " has no budget for " rules[i] ".")
    if (nfatal) return failed()
    split("", over); split("", under); split("", raised); split("", lowered)
    ndiff = judge(rules, nrules, cnt, bud, over, under)
    nover = 0
    for (r in over) nover++
    if (nover > 0 && !havebase) return 4
    if (mode == "update") {
        nlow = lower(budgetLine, nbudgetLines, cnt, lowered)
        if (nlow > 0) {
            for (i = 1; i <= nbudgetLines; i++) print budgetLine[i] > newfile
            close(newfile)
        }
        for (i = 1; i <= nrules; i++)
            if (rules[i] in lowered) say(rules[i] " " lowered[rules[i]] " -> " cnt[rules[i]])
        if (nlow == 0 && nover == 0 && nnolayer == 0) say("Every count equals its budget.")
        ndiff = nover
        split("", under)
    }
    if (mode == "check") table(over, under, ndiff)
    for (i = 1; i <= nrules; i++) if (rules[i] in over) overBlock(rules[i], over[rules[i]])
    for (i = 1; i <= nrules; i++) if (rules[i] in under) underBlock(rules[i], under[rules[i]])
    if (nnolayer > 0) noLayerBlock()
    nraise = 0
    if (mode == "staged") {
        nraise = raises(rules, nrules, bud, headMax, raised)
        if (nraise > 0) blank()
        for (i = 1; i <= nrules; i++) {
            r = rules[i]
            if (r in raised) say(r ": budget " raised[r] " in " headFrom[r] ", " bud[r] " staged.")
        }
    }
    else if (mode == "check" && raises(rules, nrules, bud, baseBud, raised) > 0) {
        blank()
        for (i = 1; i <= nrules; i++) {
            r = rules[i]
            if (r in raised)
                say(r ": the budget went up from " raised[r] " in " rev " to " bud[r] ". The commit that raises it needs the line " DQ "Architecture-Budget-Raise: " r " " raised[r] " -> " bud[r] ": <reason>" DQ " in its message (CI and pre-push check it).")
        }
    }
    return exitStatus(ndiff, nraise, nnolayer)
}

function table(over, under, ndiff,    i, r, line) {
    say("Architecture budgets (" BUDGETS ") for src/:")
    for (i = 1; i <= nrules; i++) {
        r = rules[i]
        line = padTo("    " r " ", 36 - length(cnt[r] "")) cnt[r] " of " bud[r]
        if (r in over) line = line "  " over[r] " over"
        else if (r in under) line = line "  " under[r] " under"
        say(line)
    }
    if (ndiff == 0 && nnolayer == 0) say("Every count equals its budget.")
}

function overBlock(r, n,    k, parts, keys, nk, i, old) {
    blank()
    say(r ": " cnt[r] ", " n " more than its budget of " bud[r] ".")
    say("    What counts: " what[r] ".")
    say("    Instead: " instead[r])
    if (gha) say("::error title=Architecture budget::" r " " cnt[r] ", budget " bud[r] ". " toupper(substr(instead[r], 1, 1)) substr(instead[r], 2))
    if (haveBaseCounts) {
        split("", keys)
        nk = 0
        for (k in keyCnt) {
            split(k, parts, SUBSEP)
            if (parts[1] == r && keyCnt[k] > ((k in baseKeyCnt) ? baseKeyCnt[k] : 0)) keys[++nk] = parts[2]
        }
        sortList(keys, nk)
        if (nk > 0) say("    Grown since " rev ":")
        for (i = 1; i <= nk; i++) {
            k = r SUBSEP keys[i]
            old = (k in baseKeyCnt) ? baseKeyCnt[k] : 0
            say("        " keys[i] ": " old " -> " keyCnt[k])
            if (gha) say("::error file=" keys[i] ",title=" r "::" old " -> " keyCnt[k] " since " rev)
        }
        if (nk == 0 && (r in baseBud) && baseBud[r] > bud[r])
            say("    No file has more than in " rev "; the budget is below " rev q "s (" baseBud[r] "): put it back.")
        else if (nk == 0) say("    No file has more than in " rev ".")
    }
    say("    Every match: sh tools/check-architecture.sh --list " r)
}

function underBlock(r, n) {
    blank()
    say(r ": " cnt[r] ", " n " fewer than its budget of " bud[r] ". Lower the budget in this commit:")
    say("    sh tools/check-architecture.sh --update")
    if (mode == "staged") say("    git add " BUDGETS)
    if (gha) say("::error title=Architecture budget::" r " " cnt[r] ", budget " bud[r] ". Lower the budget in the same commit with sh tools/check-architecture.sh --update.")
}

function noLayerBlock(    i) {
    blank()
    say("These C/C++ files are in no layer the architecture rules know (src/app, src/ui, src/services, src/model):")
    for (i = 1; i <= nnolayer; i++) {
        say("    " noLayer[i])
        if (gha) say("::error file=" noLayer[i] ",title=Architecture layers::This file is in no layer the architecture rules know.")
    }
    say("Move them into one of those folders, or add the new layer and its rules to tools/check-architecture.sh.")
}

# Mode range: a commit is done; report each raise that its message does not explain.
function finishCommit(    i, r) {
    if (!inCommit) return
    inCommit = 0
    ncommits++
    for (i = 1; i <= ncommitRules; i++) {
        r = commitRules[i]
        if (!(r in parentMax) || commitBud[r] <= parentMax[r] || explained(message, nmessage, r)) continue
        nunexplained++
        say(commitHash " " DQ commitSubject DQ ": " r " " parentMax[r] " -> " commitBud[r] ", and the message has no Architecture-Budget-Raise line for it.")
        if (gha) say("::error title=Architecture budget raised::" commitHash " raises " r " " parentMax[r] " -> " commitBud[r] " without an Architecture-Budget-Raise line")
    }
}

function rangeResult() {
    if (nunexplained > 0) {
        say("A budget goes up only on purpose: add " DQ "Architecture-Budget-Raise: <rule> <old> -> <new>: <reason>" DQ " to that commit" q "s message (git rebase -i, reword).")
        return 3
    }
    # Only the commits that change the budget file were read: no other commit can raise a budget.
    if (ncommits + 0 == 0) say("No commit in the range changes " BUDGETS ", so none raises a budget.")
    else if (ncommits == 1) say("1 commit in the range changes " BUDGETS ", and it raises no budget without an Architecture-Budget-Raise line.")
    else say(ncommits " commits in the range change " BUDGETS ", and none raises a budget without an Architecture-Budget-Raise line.")
    return 0
}

# ---- Self-test ----

function check(ok, what_) {
    if (ok) return
    say("    " what_)
    selfTestFailed = 1
}

function selfTest(    i, r, got, names, counts, budgets, over, under, now, before, from, raised, lines, lowered, msg) {
    for (i = 1; i <= nexpect; i++) {
        r = expectOrder[i]
        got = (r == "no-layer") ? nnolayer : total[r] + 0
        check(got == expect[r], r ": counted " got ", expected " expect[r])
    }
    check(parseBudget("a 1" CR) == 1 && bRule == "a" && bValue == 1, "a budget line ending in CR+LF doesn" q "t parse")
    check(parseBudget("# note" CR) == 0 && parseBudget("" CR) == 0, "a comment or blank line ending in CR+LF isn" q "t skipped")
    check(parseBudget("ui-undo-steps forty") == -1, "a malformed budget line is accepted")

    split("a b", names, " ")
    split("", counts); split("", budgets); split("", over); split("", under)
    counts["a"] = 1; counts["b"] = 2; budgets["a"] = 1; budgets["b"] = 3
    check(judge(names, 2, counts, budgets, over, under) == 1 && !("a" in over) && !("b" in over) && !("a" in under) && under["b"] == 1 && exitStatus(1, 0, 0) == 1, "counts a=1 b=2 with budgets a=1 b=3 don" q "t give b 1 under")
    split("", counts); split("", budgets); split("", over); split("", under)
    counts["a"] = 9; budgets["a"] = 10
    check(judge(names, 1, counts, budgets, over, under) == 1 && under["a"] == 1, "9 isn" q "t below 10")

    split("", now); split("", before); split("", from); split("", raised)
    now["a"] = 2
    keepMax(before, from, "a", 1, "HEAD")
    check(raises(names, 1, now, before, raised) == 1 && raised["a"] == 1 && exitStatus(0, 1, 0) == 3, "a=2 staged with a=1 in HEAD isn" q "t a raise")
    split("", before); split("", raised)
    check(raises(names, 1, now, before, raised) == 0, "a raise is reported without a HEAD budget file")
    keepMax(before, from, "b", 5, "HEAD")
    check(raises(names, 1, now, before, raised) == 0, "a raise is reported for a rule HEAD lacks")
    split("", before)
    keepMax(before, from, "a", 1, "HEAD")
    keepMax(before, from, "a", 2, "MERGE_HEAD")
    check(raises(names, 1, now, before, raised) == 0 && from["a"] == "MERGE_HEAD", "a raise is reported although MERGE_HEAD has a=2")

    split("", lines); split("", counts); split("", lowered)
    lines[1] = "a    10"; lines[2] = "b     3"
    counts["a"] = 7; counts["b"] = 5
    check(lower(lines, 2, counts, lowered) == 1 && lines[1] == "a     7" && lines[2] == "b     3" && lowered["a"] == 10, "--update gives " DQ lines[1] DQ " and " DQ lines[2] DQ)

    split("", msg)
    msg[1] = "Move the window hook"; msg[2] = ""; msg[3] = "Architecture-Budget-Raise: a 1 -> 2: why"
    check(explained(msg, 3, "a"), "the Architecture-Budget-Raise line for a doesn" q "t explain a raise of a")
    msg[3] = "Architecture-Budget-Raise: ab 1 -> 2"
    check(!explained(msg, 3, "a"), "an Architecture-Budget-Raise line for ab explains a raise of a")
    return selfTestFailed ? 1 : 0
}
AWK
}

# The built-in examples of the self-test: lines as git grep prints them. The self-test treats
# src/model/Tracktion.h as the prelude and src/model/Undo.cpp as the undo code.
fixture() {
    cat <<'FIXTURE'
src/model/a/Fixture.h:1:#include "../../ui/Panels.h"
src/model/a/Fixture.h:2:#include "ui/Controls.h" // why
src/model/a/Fixture.h:3:// #include "app/MainWindow.h"
src/model/a/Fixture.h:4:#  include <juce_gui_basics/juce_gui_basics.h>
src/model/a/Fixture.h:5:#include "../commands/Registry.h"
src/model/a/Fixture.h:6:/* #include <juce_gui_extra/juce_gui_extra.h>
src/model/a/Fixture.h:7:   juce::KeyPress k; */ juce::Component* c; juce::Colour col; juce::ComponentPeer* p;
src/model/a/Fixture.h:8:auto s = "juce::KeyPress"; auto e = juce::KeyPress::escapeKey;
src/model/a/Fixture.h:9:edit.getUndoManager().beginNewTransaction ("Add");
src/model/Tracktion.h:1:#include <tracktion_engine/tracktion_engine.h>
src/model/Undo.cpp:1:um.beginNewTransaction (name);
src/model/b/One.h:1:#include "Two.h"
src/model/b/Two.h:1:#include "One.h"
src/model/b/Two.cpp:1:#include "Two.h"
src/model/b/Three.cpp:1:#include "One.h"
src/services/Fixture.cpp:1:#include <juce_gui_extra/juce_gui_extra.h>
src/services/Fixture.cpp:2:#include "app/MainWindow.h"
src/services/Fixture.cpp:3:#include <tracktion_graph/tracktion_graph.h>
src/ui/Fixture.cpp:1:#include <tracktion_engine/tracktion_engine.h> // te::Edit
src/ui/Fixture.cpp:2:#include "app/MainWindow.h"
src/ui/Fixture.cpp:3:#include "model/Tracktion.h"
src/ui/Fixture.cpp:4:te::Edit* e; // te::Track
src/ui/Fixture.cpp:5:auto s = "te::x // y"; phazite::te::Clip* c; Note::Kind k; byte::x;
src/ui/Fixture.cpp:6:char q = '"'; auto n = 1'000; tracktion::engine::Edit* t;
src/ui/Fixture.cpp:7:auto r = R"x(te::A ")" )x"; std::vector<te::B*, te::C> v;
src/ui/Fixture.cpp:8:using namespace tracktion; namespace tr = tracktion; namespace te = tracktion;
src/ui/Fixture.cpp:9:using Edits = juce::Array<te::Edit*>;
src/ui/Fixture.cpp:10:undo.beginNewTransaction ("Add"); // beginNewTransaction
src/ui/Fixture.cpp:11:auto m = R"(te::E
src/ui/Fixture.cpp:12:te::F)"; te::G g; auto c = L'x'; auto u = u8'y';
src/ui/views/Fixture3.cpp:1:#include "../../model/Tracktion.h"
src/ui/views/Fixture3.cpp:2:#include "../../app/MainWindow.h"
src/app/Fixture.cpp:1:e->getUndoManager().beginNewTransaction ("Rename"); te::Edit* e2;
src/app/MainWindow.h:1:// the main window
src/Fixture.cpp:1:int x;
src/ui/Fixture.txt:1:te::NotCxx
tests/Fixture.cpp:1:#include "TestHelpers.h"
tests/Fixture.cpp:2:#include "model/b/one.h"
tests/Fixture.cpp:3:#include "model/b/One.h"
tests/TestHelpers.h:1:// test helpers
tools/x/Tool.cpp:1:#include "Missing.h"
FIXTURE
}

# The examples' CMakeLists.txt: it lists every C/C++ example in src/ and tests/ but src/model/b/Three.cpp, plus
# src/ui/fixture.cpp (wrong case) and src/ui/Gone.cpp (no such file), and src/ui/Commented.cpp only in a comment.
fixture_cmake() {
    cat <<'CMAKE'
target_sources(fixture PRIVATE
    src/model/a/Fixture.h src/model/Tracktion.h src/model/Undo.cpp
    src/model/b/One.h src/model/b/Two.h src/model/b/Two.cpp
    src/services/Fixture.cpp "src/ui/Fixture.cpp" src/ui/Fixture2.cpp src/ui/views/Fixture3.cpp
    src/app/Fixture.cpp src/app/MainWindow.h src/Fixture.cpp
    src/ui/fixture.cpp # src/ui/Commented.cpp
    "a # inside quotes" src/ui/Gone.cpp)
target_sources(fixture_tests PRIVATE tests/Fixture.cpp tests/TestHelpers.h) # tools/ needs no listing
CMAKE
}

fixture_expectations() {
    cat <<'EXPECT'
@expect upward-includes 5
@expect model-window-includes 2
@expect model-window-references 3
@expect model-tracktion-includes 1
@expect model-undo-steps 1
@expect model-include-loops 2
@expect ui-tracktion-includes 3
@expect ui-tracktion-references 8
@expect ui-tracktion-aliases 3
@expect ui-undo-steps 2
@expect flat-files 4
@expect unresolved-includes 5
@expect cmake-sources 3
@expect no-layer 1
EXPECT
}

# Runs the awk program in mode $1; the other arguments are more -v assignments.
run_awk() {
    run_mode=$1
    shift
    LC_ALL=C "$awk" -v mode="$run_mode" -v q="'" -v preludes="$tracktion_preludes" \
        -v undo_functions="$undo_functions" -v undo_implementation="$undo_implementation" -v gha="$gha" \
        "$@" "$prog"
}

# Exits with 2 if the awk program counts the examples wrong or gets the budget logic wrong. src/ui/Absent.cpp is
# listed in @files and in @absent: counted, it would add a flat file and a file CMakeLists.txt doesn't list.
self_test() {
    st_report=$( {
        printf '@absent\nsrc/ui/Absent.cpp\n\n@files\n'
        fixture | sed 's/:[0-9][0-9]*:.*$//'
        printf 'src/ui/Fixture2.cpp\nsrc/ui/Absent.cpp\n\n@tree\n'
        fixture
        printf 'src/ui/Fixture2.cpp:1:te::D d;\r\n'
        printf '\n@status 0\n\n@cmake\n'
        fixture_cmake | sed 's/^/=/'
        printf '\n'
        fixture_expectations
    } | run_awk self-test -v preludes=src/model/Tracktion.h -v undo_implementation=src/model/Undo.cpp \
        -v undo_functions=beginNewTransaction)
    [ $? -eq 0 ] && return 0
    printf '%s\n' "tools/check-architecture.sh: this awk ($awk) counts the built-in examples wrong:" "$st_report" >&2
    exit 2
}

# The input sections for the code in $1: the working tree (worktree), the index (index) or a commit.
code_sections() {
    # Files that git ls-files lists but the code doesn't have: in the working tree, those deleted from the disk;
    # in the index, those that git add -N only marked (git diff-files shows them as added), which git grep
    # --cached skips and git commit leaves out.
    printf '@absent\n'
    case $1 in
        worktree) git -c core.quotepath=off ls-files --deleted -- src tests tools ;;
        index) git -c core.quotepath=off diff-files --name-only --diff-filter=A -- src tests tools ;;
    esac
    printf '\n@files\n'
    case $1 in
        worktree) git -c core.quotepath=off ls-files --cached --others --exclude-standard -- src tests tools ;;
        index) git -c core.quotepath=off ls-files --cached -- src tests tools ;;
        *) git -c core.quotepath=off ls-tree -r --name-only "$1" -- src tests tools ;;
    esac
    printf '\n@tree\n'
    case $1 in
        worktree) git -c core.quotepath=off grep --no-color --no-column -F -n -I -e '' --untracked -- src tests tools ;;
        index) git -c core.quotepath=off grep --no-color --no-column -F -n -I -e '' --cached -- src tests tools ;;
        *) git -c core.quotepath=off grep --no-color --no-column -F -n -I -e '' "$1" -- src tests tools ;;
    esac
    printf '\n@status %s\n\n@cmake\n' "$?"
    case $1 in
        worktree) sed 's/^/=/' CMakeLists.txt ;;
        index) git cat-file blob :CMakeLists.txt | sed 's/^/=/' ;;
        *) git cat-file blob "$1:CMakeLists.txt" | sed 's/^/=/' ;;
    esac
    printf '\n'
}

# Whether $1 (worktree, index or a commit) has a budget file.
has_budgets() {
    case $1 in
        worktree) [ -f "$budget_file" ] ;;
        index) git cat-file -e ":$budget_file" 2>/dev/null ;;
        *) git cat-file -e "$1:$budget_file" 2>/dev/null ;;
    esac
}

# Prints the budget file of $2 (worktree, index or a commit) as section $1 with argument $3, or nothing if $2
# has none.
budget_section() {
    has_budgets "$2" || return 0
    printf '@%s %s\n' "$1" "${3:-}"
    case $2 in
        worktree) sed 's/^/=/' "$budget_file" ;;
        index) git cat-file blob ":$budget_file" | sed 's/^/=/' ;;
        *) git cat-file blob "$2:$budget_file" | sed 's/^/=/' ;;
    esac
    printf '\n'
}

# The commits that the staged merge joins to HEAD: those in MERGE_HEAD when git commit finishes a merge. When
# git merge makes the commit itself, it runs the pre-merge-commit hook before it writes MERGE_HEAD, but it names
# each merged commit in a variable GITHEAD_<hash>.
merge_heads() {
    mh_file=$(git rev-parse --git-path MERGE_HEAD)
    if [ -f "$mh_file" ]; then
        cat "$mh_file"
    else
        env | sed -n 's/^GITHEAD_\([0-9a-f][0-9a-f]*\)=.*/\1/p'
    fi
}

# Exits with 2 if a prelude is missing from $1 (worktree or index).
check_preludes() {
    for cp_prelude in $tracktion_preludes; do
        case $1 in
            index) git cat-file -e ":$cp_prelude" 2>/dev/null ;;
            *) [ -f "$cp_prelude" ] ;;
        esac && continue
        echo "tools/check-architecture.sh lists $cp_prelude as a Tracktion prelude, but there is no such file." >&2
        exit 2
    done
}

# Counts the code in $1 (worktree or index) and reports in mode $2 (check, update or staged), comparing with
# the commit $3 (none if empty), shown as $4. Returns the exit code.
check_code() {
    cc_source=$1 cc_mode=$2 cc_base=$3 cc_name=$4
    if ! has_budgets "$cc_source"; then
        if [ "$cc_source" = index ]; then
            echo "tools/check-architecture.sh: $budget_file isn't staged, so there are no budgets to check." >&2
        else
            echo "tools/check-architecture.sh: there is no $budget_file, so there are no budgets to check." >&2
        fi
        return 2
    fi
    check_preludes "$cc_source"
    cc_counts=$(code_sections "$cc_source" | run_awk count) || {
        printf '%s\n' "$cc_counts" >&2
        return 2
    }
    cc_have=1 cc_base_counts=
    [ -n "$cc_base" ] && cc_have=0
    rm -f "$budget_file.new"
    while :; do
        cc_report=$( {
            budget_section budgets "$cc_source"
            if [ "$cc_mode" = staged ]; then
                [ -z "$cc_base" ] || budget_section head-budgets "$cc_base" HEAD
                for cc_merge in $(merge_heads); do
                    budget_section head-budgets "$cc_merge" MERGE_HEAD
                done
            elif [ -n "$cc_base" ]; then
                budget_section base-budgets "$cc_base" "$cc_name"
            fi
            printf '@counts\n%s\n\n' "$cc_counts"
            [ -z "$cc_base_counts" ] || printf '@base-counts\n%s\n\n' "$cc_base_counts"
        } | run_awk "$cc_mode" -v havebase="$cc_have" -v rev="$cc_name" -v newfile="$budget_file.new")
        cc_status=$?
        [ $cc_status -eq 4 ] || break
        cc_base_counts=$(code_sections "$cc_base" | run_awk count -v prefix="$cc_base") || {
            printf '%s\n' "$cc_base_counts" >&2
            return 2
        }
        cc_have=1
    done
    if [ $cc_status -eq 2 ]; then
        rm -f "$budget_file.new"
        printf '%s\n' "$cc_report" >&2
        return 2
    fi
    if [ -f "$budget_file.new" ]; then
        mv -f "$budget_file.new" "$budget_file" || return 2
    fi
    [ -z "$cc_report" ] || printf '%s\n' "$cc_report"
    # Files that git add -N only marked are on the disk but not in the commit, which can puzzle: name them.
    if [ "$cc_mode" = staged ] && [ $cc_status -eq 1 ]; then
        cc_marked=$(git -c core.quotepath=off diff-files --name-only --diff-filter=A -- src tests tools | sed 's/^/    /')
        [ -z "$cc_marked" ] || printf '\n%s\n%s\n' \
            "These files are only marked with git add -N, so this commit leaves them out; git add stages them:" "$cc_marked"
    fi
    return $cc_status
}

# Mode --list: every match of rule $1 in the working tree.
list_rule() {
    check_preludes worktree
    lr_report=$(code_sections worktree | run_awk list -v listing="$1") || {
        printf '%s\n' "$lr_report" >&2
        return 2
    }
    printf '%s\n' "$lr_report"
}

# Mode --range: every commit of "git rev-list $1" whose budget is above all its parents' must say why. Only
# commits that change the budget file can raise it, so only those are read.
range_check() {
    set -f
    rc_commits=$(git rev-list --reverse --full-history $1 -- "$budget_file") || {
        set +f
        echo "tools/check-architecture.sh: git rev-list couldn't list the commits of \"$1\"." >&2
        return 2
    }
    set +f
    rc_report=$(for rc_commit in $rc_commits; do
        printf '@commit\n'
        git log -1 --format='%h %s%n%B' "$rc_commit" | sed 's/^/=/'
        printf '\n@commit-budgets\n'
        git cat-file blob "$rc_commit:$budget_file" 2>/dev/null | sed 's/^/=/'
        for rc_parent in $(git log -1 --format=%P "$rc_commit"); do
            printf '\n@parent-budgets\n'
            git cat-file blob "$rc_parent:$budget_file" 2>/dev/null | sed 's/^/=/'
        done
        printf '\n'
    done | run_awk range)
    rc_status=$?
    if [ $rc_status -eq 2 ]; then
        printf '%s\n' "$rc_report" >&2
        return 2
    fi
    printf '%s\n' "$rc_report"
    return $rc_status
}

# Mode --ci: the check compared with where this run's changes start, then the raises since then.
ci() {
    ci_base= ci_why= ci_first=yes
    set -f
    for ci_candidate in ${PHAZITE_CI_BASES:-}; do
        ci_label="the commit before this push"
        if [ $ci_first = yes ]; then
            case ${GITHUB_EVENT_NAME:-} in
                pull_request*) ci_label="the pull request's base" ;;
            esac
        fi
        ci_first=no
        if git cat-file -e "$ci_candidate^{commit}" 2>/dev/null &&
            git merge-base --is-ancestor "$ci_candidate" HEAD 2>/dev/null; then
            ci_base=$(git rev-parse "$ci_candidate^{commit}") ci_why=$ci_label
            break
        fi
    done
    set +f
    if [ -z "$ci_base" ]; then
        ci_main=$(git merge-base HEAD origin/main 2>/dev/null)
        if [ -n "$ci_main" ] && [ "$ci_main" != "$(git rev-parse HEAD)" ]; then
            ci_base=$ci_main ci_why="where this branch left main"
        elif ci_main=$(git rev-parse -q --verify 'HEAD^' 2>/dev/null); then
            ci_base=$ci_main ci_why="the commit before HEAD"
        fi
    fi
    if [ -z "$ci_base" ]; then
        echo "There is no commit to compare with: HEAD is the first commit."
        echo
        check_code worktree check '' ''
        return $?
    fi
    ci_short=$(git rev-parse --short "$ci_base")
    echo "Compared with $ci_short ($ci_why)."
    echo
    check_code worktree check "$ci_base" "$ci_short"
    ci_check=$?
    echo
    range_check "$ci_base..HEAD"
    ci_range=$?
    if [ $ci_check -eq 2 ] || [ $ci_range -eq 2 ]; then return 2; fi
    [ $ci_check -eq 0 ] || return 1
    return $ci_range
}

if [ "$mode" = help ]; then
    sed -n -e '1d' -e '/^#/!q' -e 's/^# \{0,1\}//p' tools/check-architecture.sh
    exit 0
fi

# Nothing that the counts or the budgets depend on is staged.
if [ "$mode" = staged ] &&
    git diff --cached --quiet -- src tests tools CMakeLists.txt tools/check-architecture.sh "$budget_file"; then
    exit 0
fi

if ! command -v "$awk" >/dev/null 2>&1; then
    echo "tools/check-architecture.sh: there is no $awk to run (set AWK to a POSIX awk)." >&2
    exit 2
fi
prog=$(awk_program)
self_test

head=$(git rev-parse -q --verify 'HEAD^{commit}')
case $mode in
    self-test) echo "The self-test passed: $awk counts the built-in examples as expected." ;;
    check)
        if [ -z "$base" ]; then
            check_code worktree check "$head" HEAD
        elif base_commit=$(git rev-parse -q --verify "$base^{commit}"); then
            check_code worktree check "$base_commit" "$base"
        else
            echo "tools/check-architecture.sh: $base isn't a commit." >&2
            exit 2
        fi ;;
    update) check_code worktree update "$head" HEAD ;;
    staged) check_code index staged "$head" HEAD ;;
    list) list_rule "$rule" ;;
    range) range_check "$range" ;;
    ci) ci ;;
esac
