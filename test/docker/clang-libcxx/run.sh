#!/bin/sh
#
# Builds GNUWorld with Clang and libc++, in a container, and runs the C++ unit
# tests there.  A COMPILE gate: FreeBSD is a deployment target and its C++
# standard library is libc++, while the machine this is usually developed on has
# libstdc++ alone - so a difference between the two is invisible here until it is
# a bug report (see the Dockerfile beside this, which tells the std::endl story).
#
# Run it from the repository root:
#
#     test/docker/clang-libcxx/run.sh
#
# What it does: builds the image of the Dockerfile beside it, then streams the
# WORKING TREE's tracked files into a container of that image through a tar on
# stdin - so uncommitted edits are included, and not one build product of the
# host is.  Nothing is mounted, and nothing this writes reaches the host; when it
# is done the only thing it leaves behind is the image named below.
#
# Warnings are COUNTED and printed, not fatal: Clang warns about things GCC does
# not, and a gate that failed on every one of them would be turned off in a week.
# Errors - a compile error, a failing test, a binary that turns out to be linked
# against libstdc++ after all - fail it.
#
# Its exit status is the exit status of everything it ran inside.

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
image=gnuworld-clang-libcxx

# -e and not -d: in a git worktree ".git" is a file naming the real one
if [ ! -f configure.ac ] || [ ! -e .git ]; then
    printf 'run.sh: run this from the repository root: test/docker/clang-libcxx/run.sh\n' >&2
    exit 2
fi

printf '=== Building the image %s\n' "$image"
docker build -t "$image" "$here"

# What runs INSIDE the container.  It is passed as the argument of "sh -c" and
# not baked into the image, because stdin belongs to the tar of the tree: this
# way the whole gate is one file to read.
#
# The module list is the full one of the ordinary build.  Not one module had to
# be dropped for the missing libraries: prometheus-cpp, log4cplus and libcurl are
# each reached only through ENABLE_LOG4CPLUS, HAVE_PROMETHEUS or HAVE_LIBCURL,
# which configure simply does not define here - mod.dronescan, mod.cservice and
# libnotifier all compile without them.  What does not get compiled in this image
# is the code behind those three, which is what test/docker/full-deps is for.
inside=$(cat <<'INSIDE'
set -u

readonly MODULES='debug,gnutest,ccontrol,cloner,dronescan,nickserv,openchanfix,cservice,scanner,clientExample,snoop,stats'
readonly BUILD_LOG=/tmp/build.log

failed=0

say() { printf '\n=== %s\n' "$*"; }

fail() {
    printf '*** FAILED: %s\n' "$*"
    failed=1
}

# ---------------------------------------------------------------- the tree

say 'Unpacking the working tree'
tar -xf - -C /src || {
    printf '*** FAILED: could not unpack the tree from stdin\n'
    exit 1
}
printf 'unpacked %s files\n' "$(find /src -type f | wc -l)"

cd /src || exit 1

# ---------------------------------------------------------------- configure

say 'autogen.sh'
./autogen.sh > /tmp/autogen.log 2>&1 || {
    tail -40 /tmp/autogen.log
    printf '*** FAILED: autogen.sh\n'
    exit 1
}

say "configure --enable-modules=$MODULES --with-test, with clang++ and libc++"
clang++ --version | head -1
./configure --enable-modules="$MODULES" --with-test \
    CXX=clang++ CXXFLAGS="-stdlib=libc++" LDFLAGS="-stdlib=libc++" \
    > /tmp/configure.log 2>&1
configure_status=$?

# The three that are not in this image, said out loud: configure only warns
# about them, and a warning is easy to read past
grep -iE 'libcurl|prometheus' /tmp/configure.log

if [ $configure_status -ne 0 ]; then
    tail -40 /tmp/configure.log
    printf '*** FAILED: configure\n'
    exit 1
fi

# ---------------------------------------------------------------- build

# configure.ac ASSIGNS CXXFLAGS outright ("CXXFLAGS=\"-W -Wall -pipe ...\"") and
# so throws away the -stdlib=libc++ passed to configure a moment ago; LDFLAGS it
# only ever appends to.  Left at that, every file is COMPILED against libstdc++
# and the whole thing is LINKED against libc++, which ends in a page of undefined
# references to std::__cxx11 names.  So the flag is said again here, on make's
# own command line where nothing can drop it, together with the value configure
# settled on - that value carries the include paths the tree needs.
configured_cxxflags=$(sed -n 's/^CXXFLAGS = //p' Makefile | head -1)
printf 'configure settled on CXXFLAGS = %s\n' "$configured_cxxflags"
printf 'make is given CXXFLAGS = %s -stdlib=libc++\n' "$configured_cxxflags"

say 'make -j6'
make -j6 CXXFLAGS="$configured_cxxflags -stdlib=libc++" > "$BUILD_LOG" 2>&1
build_status=$?

if [ $build_status -ne 0 ]; then
    grep -E 'error:' "$BUILD_LOG" | head -40
    tail -40 "$BUILD_LOG"
    printf '*** FAILED: make\n'
    exit 1
fi

warnings=$(grep -c 'warning:' "$BUILD_LOG" || true)
printf 'warnings in the whole build: %s\n' "$warnings"

if [ "$warnings" != 0 ]; then
    printf 'what they are, by kind (counted, not fatal):\n'
    grep -o '\[-W[a-z0-9-]*\]' "$BUILD_LOG" | sort | uniq -c | sort -rn | head -20
    # Every distinct warning, with how many translation units saw it.  A header
    # warning is reported once per TU that includes the header, so a two-line
    # mistake in a header with 67 includers counts as 134; without this list you
    # cannot tell that from 134 separate mistakes, and the log dies with the
    # container.
    printf 'each distinct warning, and how many times it was reported:\n'
    grep 'warning:' "$BUILD_LOG" | sed 's/^.*\/\([^/]*:[0-9]*\):[0-9]*: warning:/\1: warning:/' \
        | sort | uniq -c | sort -rn
fi

# ---------------------------------------------------- really libc++, then?

# The whole point of the image, asserted rather than assumed: a stray
# -stdlib=libstdc++ anywhere, or a libtool link line that lost the flag, would
# make every other check here meaningless.
say 'What the daemon is linked against'

binary=./gnuworld
[ -f .libs/gnuworld ] && binary=.libs/gnuworld

ldd "$binary" | grep -E 'libc\+\+|libstdc\+\+' || printf 'neither: %s\n' "$binary"

if ldd "$binary" | grep -q 'libstdc++'; then
    fail "$binary is linked against libstdc++, so this was not a libc++ build"
fi

if ! ldd "$binary" | grep -q 'libc++'; then
    fail "$binary is linked against no libc++ at all"
fi

# ---------------------------------------------------------------- the tests

say 'make check'
make check > /tmp/check.log 2>&1
check_status=$?

grep -E '^# (TOTAL|PASS|SKIP|XFAIL|FAIL|XPASS|ERROR)' /tmp/check.log

if [ $check_status -ne 0 ]; then
    sed -n '/FAIL:/,$p' /tmp/check.log | head -20
    # What the failing program itself said, which is the whole point: a test that
    # passes with libstdc++ and fails here has found the difference this gate is
    # for, and the container is gone the moment this script ends
    printf '\n---- test-suite.log\n'
    cat test-suite.log 2>/dev/null | head -120
    fail 'make check'
fi

# ---------------------------------------------------------------- the verdict

say 'Summary'

if [ $failed -eq 0 ]; then
    printf 'ALL GOOD: the tree builds with clang++ and libc++ (%s warnings),\n' "$warnings"
    printf 'the daemon is linked against libc++, and make check passed.\n'
else
    printf 'SOMETHING FAILED, see above.\n'
fi

exit $failed
INSIDE
)

printf '=== Streaming the working tree into a container\n'
printf '    %s tracked files\n' "$(git ls-files | wc -l)"

# --null/-T -: the file list comes from git, so a name with a space in it is one
# name; -i keeps stdin open for the tar the container unpacks.
git ls-files -z | tar --null -T - -cf - | docker run --rm -i "$image" \
    /bin/sh -c "$inside"
