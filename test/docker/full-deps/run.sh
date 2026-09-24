#!/bin/sh
#
# Builds GNUWorld with every optional dependency, in a container, and runs every
# test there - including the ones that only exist where libcurl and
# prometheus-cpp are (the pushover sink's delivery test, prometheus.cc itself,
# cservice's HAVE_PROMETHEUS blocks).
#
# Run it from the repository root:
#
#     test/docker/full-deps/run.sh
#
# What it does: builds the image of the Dockerfile beside it, then streams the
# WORKING TREE's tracked files into a container of that image through a tar on
# stdin - so uncommitted edits are included, and not one build product of the
# host is.  Nothing is mounted, and nothing this writes reaches the host.
#
# Its exit status is the exit status of everything it ran inside (see
# build-and-test, which is the script the container runs).

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
image=gnuworld-full-deps

# -e and not -d: in a git worktree ".git" is a file naming the real one
if [ ! -f configure.ac ] || [ ! -e .git ]; then
    printf 'run.sh: run this from the repository root: test/docker/full-deps/run.sh\n' >&2
    exit 2
fi

printf '=== Building the image %s\n' "$image"
docker build -t "$image" "$here"

printf '=== Streaming the working tree into a container\n'
printf '    %s tracked files\n' "$(git ls-files | wc -l)"

# --null/-T -: the file list comes from git, so a name with a space in it is one
# name; -i keeps stdin open for the tar the container unpacks.
git ls-files -z | tar --null -T - -cf - | docker run --rm -i "$image" \
    /usr/local/bin/build-and-test
