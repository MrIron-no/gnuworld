#!/bin/sh
#
# Builds GNUWorld with AddressSanitizer and UndefinedBehaviorSanitizer and runs
# every test under them - the C++ unit tests of "make check" and the whole
# integration harness, whose daemon inherits the sanitizer options from here.
#
# Run it from the repository root:
#
#     test/sanitize/run.sh                       # everything
#     test/sanitize/run.sh --keep                # and keep the exported tree
#     test/sanitize/run.sh -- test_logging.py    # one harness file only
#
# Everything after "--" is passed to pytest.
#
# What it does: exports the WORKING TREE's tracked files into a fresh temporary
# directory under ${TMPDIR:-/tmp} - so uncommitted edits are included, and not
# one build product of the host is - then configures, builds and tests there.
# Nothing this writes reaches the repository, and the ordinary build of the
# checkout is left alone: a sanitized object file next to an unsanitized one is
# a link error waiting to happen.  No container is involved; this gate uses the
# host toolchain, and needs Docker only for the Postgres the harness wants.
#
# A sanitizer says nothing through an exit status: a daemon the harness stops is
# indistinguishable from one ASan aborted.  So both sanitizers are told to write
# their reports to files ("log_path" below), and this script fails if any such
# file exists at the end, whatever every step's exit status was.
#
# Its exit status: 0 only if every step passed AND no sanitizer report was
# written; 1 otherwise; 2 if it was run from the wrong place or misused.

set -eu

# -e and not -d: in a git worktree ".git" is a file naming the real one
if [ ! -f configure.ac ] || [ ! -e .git ]; then
    printf 'run.sh: run this from the repository root: test/sanitize/run.sh\n' >&2
    exit 2
fi

readonly MODULES='debug,gnutest,ccontrol,cloner,dronescan,nickserv,openchanfix,cservice,scanner,clientExample,snoop,stats'
readonly SANITIZE_CXXFLAGS='-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined'
readonly SANITIZE_LDFLAGS='-fsanitize=address,undefined'

# The one check of the twenty-odd in "undefined" that this tree cannot be built
# with, and it is a link error rather than a report: UBSan's vptr check emits a
# reference to the typeinfo of every polymorphic type a pointer is used through,
# and libgnuworld/match.cc takes a "const iClient*" - a type of the CORE, whose
# typeinfo lives in libgnuworldcore.  test_burst and test_mtrie_load link
# libgnuworld alone, deliberately, so they end in
#
#     undefined reference to `typeinfo for gnuworld::iClient'
#
# and nothing at all gets built.  Take this out the day libgnuworld stops naming
# a core type in its own headers; do not widen it to anything else.
readonly SANITIZE_EXCLUDE='-fno-sanitize=vptr'

usage() {
    printf 'usage: test/sanitize/run.sh [--keep] [-- <pytest arguments>]\n'
}

keep=no

while [ $# -gt 0 ]; do
    case $1 in
        --keep)
            keep=yes
            ;;
        --)
            shift
            break
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'run.sh: unknown option: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

dir=$(mktemp -d "${TMPDIR:-/tmp}/gnuworld-sanitize.XXXXXX")

failed=0

say() { printf '\n=== %s\n' "$*"; }

fail() {
    printf '*** FAILED: %s\n' "$*"
    failed=1
}

# detect_leaks=0 IS DELIBERATE, for now.  A survey of core found leaks that are
# still there at exit - objects the daemon allocates once and never gives back -
# and later parts of the core overhaul are what fix them.  Leak checking today
# would bury every real ASan finding under them, so this gate watches for the
# errors that are bugs wherever they happen (overflows, use-after-free,
# undefined behaviour) and leaves the leaks to the tasks that own them.  Turn
# this to 1 once those are done.
#
# abort_on_error/halt_on_error: stop at the first error, so a report is about one
# thing; log_path: the reports go to files this script looks for at the end,
# because an exit status does not survive a harness that stops its daemon.
#
# suppressions: test/sanitize/asan.supp, of the exported tree, so the gate reads
# the tree's own file rather than the checkout's.  It holds eight entries, all of
# them ODR violations that abort the daemon before main() - read it, they are a
# finding and not a dismissal.
ASAN_OPTIONS="detect_leaks=0:abort_on_error=1:halt_on_error=1:log_path=$dir/asan:suppressions=$dir/test/sanitize/asan.supp"
UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1:log_path=$dir/ubsan"
export ASAN_OPTIONS UBSAN_OPTIONS

printf '=== A sanitized build of the working tree\n'
printf '    tree:    %s tracked files\n' "$(git ls-files | wc -l)"
printf '    into:    %s\n' "$dir"
printf '    ASAN_OPTIONS=%s\n' "$ASAN_OPTIONS"
printf '    UBSAN_OPTIONS=%s\n' "$UBSAN_OPTIONS"

# --null/-T -: the file list comes from git, so a name with a space in it is one
# name.  Tracked files only: an untracked build product of the host in there
# would be linked against the unsanitized libraries it was built with.
say 'Exporting the tracked tree'
git ls-files -z | tar --null -T - -cf - | tar -xf - -C "$dir"
printf 'exported %s files\n' "$(find "$dir" -type f | wc -l)"

cd "$dir"

say 'autogen.sh'
if ! ./autogen.sh > "$dir/autogen.log" 2>&1; then
    tail -40 "$dir/autogen.log"
    fail 'autogen.sh'
fi

if [ $failed -eq 0 ]; then
    say "configure --enable-modules=$MODULES --with-test"
    if ! ./configure --enable-modules="$MODULES" --with-test \
            CXXFLAGS="$SANITIZE_CXXFLAGS $SANITIZE_EXCLUDE" \
            LDFLAGS="$SANITIZE_LDFLAGS" \
            > "$dir/configure.log" 2>&1; then
        tail -40 "$dir/configure.log"
        fail 'configure'
    fi
fi

# configure.ac ASSIGNS CXXFLAGS outright ("CXXFLAGS=\"-W -Wall -pipe ...\"") and
# so throws away whatever was passed to configure a moment ago; LDFLAGS it only
# ever appends to.  A sanitized build of this tree therefore needs the compile
# flags said again where nothing can drop them - on make's own command line,
# which overrides the Makefile - and said together with the value configure
# settled on, because that value carries the include paths the tree needs.
#
# Without this the build links libasan and libubsan and instruments NOTHING: the
# runtime's malloc interceptors and its SIGSEGV handler still report, so such a
# run looks like it is working, while every buffer overflow and every undefined
# shift in the tree goes unseen.  The assertion after the build is what says so.
if [ $failed -eq 0 ]; then
    configured_cxxflags=$(sed -n 's/^CXXFLAGS = //p' Makefile | head -1)
    printf 'configure settled on CXXFLAGS = %s\n' "$configured_cxxflags"
    printf 'make is given CXXFLAGS = %s %s %s\n' \
        "$configured_cxxflags" "$SANITIZE_CXXFLAGS" "$SANITIZE_EXCLUDE"

    say 'make -j6'
    if ! make -j6 CXXFLAGS="$configured_cxxflags $SANITIZE_CXXFLAGS $SANITIZE_EXCLUDE" \
            > "$dir/build.log" 2>&1; then
        grep -E 'error:' "$dir/build.log" | head -40 || true
        tail -40 "$dir/build.log"
        fail 'make'
    else
        printf 'warnings in the whole build: %s\n' \
            "$(grep -c 'warning:' "$dir/build.log" || true)"
    fi
fi

# The instrumentation, asserted rather than assumed: an instrumented object file
# calls into the runtime, so the core library has undefined __asan_ and __ubsan_
# references.  None of them means the whole run below is worth nothing.
if [ $failed -eq 0 ]; then
    say 'Is it really instrumented?'
    asan_refs=$(nm -uD .libs/libgnuworldcore.so.0 2>/dev/null |
        grep -c '__asan_\|__ubsan_' || true)
    printf 'libgnuworldcore.so.0 has %s undefined sanitizer references\n' "$asan_refs"

    if [ "$asan_refs" = 0 ]; then
        fail 'nothing was instrumented: the sanitizer flags did not reach the compiler'
    fi
fi

if [ $failed -eq 0 ]; then
    say 'make check'
    if ! make check > "$dir/check.log" 2>&1; then
        sed -n '/FAIL:/,$p' "$dir/check.log" | head -60
        fail 'make check'
    fi
    grep -E '^# (TOTAL|PASS|SKIP|XFAIL|FAIL|XPASS|ERROR)' "$dir/check.log" || true
fi

# The harness runs the gnuworld of the tree it sits in, which is the sanitized
# one here, and the daemon it spawns inherits ASAN_OPTIONS and UBSAN_OPTIONS
# from this environment.  It is run whenever there is a daemon to run, even if
# "make check" failed: the two look at different code, and a report from each is
# worth more than the first one alone.
if [ -f "$dir/gnuworld" ]; then
    say 'the integration harness'
    # Through a pipe so that it is both watchable and kept: harness.log is one of
    # the files searched for a UBSan report below.  The exit status is carried out
    # of the subshell in a file, because a pipeline's status is the last command's.
    (
        cd "$dir/test/harness" || exit 1
        python3 -m pytest -q --timeout=180 "$@" 2>&1
        printf '%s\n' "$?" > "$dir/harness.status"
    ) | tee "$dir/harness.log"

    if [ "$(cat "$dir/harness.status" 2>/dev/null)" != 0 ]; then
        fail 'the integration harness'
    fi
else
    printf 'no gnuworld was built: the harness is skipped\n'
fi

# ---------------------------------------------------------------- the reports

# Two places to look, because the two runtimes do not agree on where to write.
# ASan honours log_path, so each of its reports is a file beside the tree.  GCC's
# libubsan IGNORES log_path (checked, GCC 14) and always writes to stderr - so
# every step's output is captured to a log here, and the logs are searched for
# UBSan's own first line, "runtime error:".
#
# One case escapes both, and it is worth knowing about: the daemon's stderr.  The
# harness merges it into the pipe it reads and does not print it, so a UBSan
# finding INSIDE the daemon shows up only as a failing harness test, with nothing
# in any log.  test/README.sanitize says how to get the message out of one.
say 'Sanitizer reports'

reports=$(find "$dir" -maxdepth 1 \( -name 'asan.*' -o -name 'ubsan.*' \) | sort)

if [ -n "$reports" ]; then
    printf '%s\n' "$reports" | while IFS= read -r report; do
        printf '\n---- %s\n' "$report"
        cat "$report"
    done
    fail 'a sanitizer wrote a report'
else
    printf 'no %s/asan.* and no %s/ubsan.* file was written\n' "$dir" "$dir"
fi

said=$(grep -l 'runtime error:' "$dir"/*.log 2>/dev/null || true)

if [ -n "$said" ]; then
    printf '%s\n' "$said" | while IFS= read -r log; do
        printf '\n---- UndefinedBehaviorSanitizer, on the stderr captured in %s\n' "$log"
        grep -A 12 'runtime error:' "$log" | head -60
    done
    fail 'UBSan reported undefined behaviour'
else
    printf 'and no captured log holds a UBSan "runtime error:"\n'
fi

# ---------------------------------------------------------------- the verdict

say 'Summary'

if [ $failed -eq 0 ]; then
    printf 'ALL GOOD: the tree builds with ASan and UBSan, make check passed,\n'
    printf 'the harness passed and neither sanitizer said anything.\n'
else
    printf 'SOMETHING FAILED, see above.\n'
fi

if [ "$keep" = yes ]; then
    printf 'the exported tree is kept (--keep): %s\n' "$dir"
elif [ $failed -eq 0 ]; then
    cd /
    rm -rf "$dir"
else
    printf 'the exported tree is kept, to look at: %s\n' "$dir"
fi

exit $failed
