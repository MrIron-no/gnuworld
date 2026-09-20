# An image with Clang and libc++ and NOTHING of GCC's C++ standard library, so
# that GNUWorld is really compiled against the other of the two standard
# libraries it has to work with.  FreeBSD is a deployment target and its C++ is
# libc++, while the machine this is usually developed on has libstdc++ alone -
# so a difference between the two is invisible here until it is a bug report.
#
# It has happened: code that compared the ADDRESS of std::endl to decide whether
# a stream manipulator was the end of a line.  libstdc++ gave one address per
# program and the comparison worked; libc++ instantiates the template in each
# translation unit, so the addresses differed and the comparison silently never
# matched.  It compiled cleanly on both.
#
# This gate is therefore a COMPILE gate: what it proves is that the tree builds
# and that its unit tests pass with Clang and libc++.  It is not the
# full-dependency image (see test/docker/full-deps/Dockerfile) and it is not
# meant to be: nothing of the source tree is baked in here either, because
# test/docker/clang-libcxx/run.sh streams the working tree's tracked files into
# a container of this image.
FROM debian:trixie

# clang, libc++-dev, libc++abi-dev: the compiler and the standard library this
# image exists for; make..pkg-config: the autotools GNUWorld builds with;
# libltdl-dev: the module loader; libpq-dev: pg_config and libpq, which configure
# demands for the database modules; libssl-dev: the TLS the server links;
# libpcre2-dev: what configure insists on for mod.dronescan; liboath-dev: the
# TOTP it insists on for mod.cservice; python3: the harness's language, here so
# that a script of the tree can run at all.
#
# DELIBERATELY ABSENT: prometheus-cpp, log4cplus and libcurl.  All three are C++
# libraries, and the Debian ones are built against libstdc++: linking them into a
# libc++ program mixes two incompatible std:: ABIs, and what you get is either a
# link error about a mangled name or a crash in a std::string nobody wrote.  The
# code behind HAVE_PROMETHEUS and HAVE_LIBCURL is what test/docker/full-deps
# exists to compile; this image compiles everything else, with the other standard
# library.  configure only warns about all three, so their absence is not an
# error here - it is the point.
RUN apt-get update && apt-get install -y --no-install-recommends \
        clang \
        libc++-dev \
        libc++abi-dev \
        make \
        autoconf \
        automake \
        libtool \
        libltdl-dev \
        pkg-config \
        libpq-dev \
        libssl-dev \
        libpcre2-dev \
        liboath-dev \
        python3 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Said here rather than found missing halfway through a build of GNUWorld: this
# image is worth nothing if clang++ cannot compile against libc++ at all, and
# <format> is the one header configure.ac refuses to go on without (it needs
# Clang >= 17 with libc++).
RUN printf '#include <format>\n#include <string>\nint main(){return std::format("{}", 1).size() - 1;}\n' \
        > /tmp/probe.cc \
    && clang++ -std=c++20 -stdlib=libc++ /tmp/probe.cc -o /tmp/probe \
    && /tmp/probe \
    && clang++ --version | head -1 \
    && rm -f /tmp/probe.cc /tmp/probe

# These three must NOT be findable, or this image is quietly the full-deps one
# with a different compiler and proves nothing about libc++
RUN ! pkg-config --exists libcurl \
    && ! pkg-config --exists prometheus-cpp-core \
    && [ ! -f /usr/include/log4cplus/logger.h ] \
    && printf 'no libcurl, no prometheus-cpp, no log4cplus: as intended\n'

WORKDIR /src

# The tree arrives on stdin as a tar stream, and the script that unpacks it is
# passed in by test/docker/clang-libcxx/run.sh - there is nothing to bake in
ENTRYPOINT []
CMD ["/bin/sh", "-c", "printf 'run this through test/docker/clang-libcxx/run.sh\\n' >&2; exit 2"]
