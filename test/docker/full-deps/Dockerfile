# An image with every optional dependency GNUWorld's configure looks for, so
# that the code behind HAVE_LIBCURL and HAVE_PROMETHEUS is really compiled and
# really run - which the machine this is usually developed on cannot do.
#
# Nothing of the source tree is baked in: test/docker/full-deps/run.sh streams
# the working tree's tracked files into a container of this image, so what is
# built is what the tree says right now and no build product of the host.
FROM debian:trixie-slim

# build-essential..pkg-config: the toolchain and the autotools GNUWorld builds
# with; libltdl-dev: the module loader; libpq-dev: pg_config and libpq, which
# configure needs for the database modules; libssl-dev: the TLS the server
# links; liboath-dev: the TOTP configure insists on once it is looked for;
# libcurl4-openssl-dev: the first of the two optional dependencies this image
# exists for; python3: the endpoint the delivery test posts to.
#
# cmake, libcivetweb-dev and curl are here for prometheus-cpp, which is built
# below: see the comment there.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        autoconf \
        automake \
        libtool \
        libltdl-dev \
        pkg-config \
        libpq-dev \
        libpcre2-dev \
        libssl-dev \
        liboath-dev \
        libcurl4-openssl-dev \
        zlib1g-dev \
        python3 \
        ca-certificates \
        cmake \
        curl \
        libcivetweb-dev \
        civetweb \
    && rm -rf /var/lib/apt/lists/*

# prometheus-cpp, built from source, because DEBIAN SHIPS NO C++ PROMETHEUS
# CLIENT AT ALL: "libprometheus-cpp-dev" does not exist in trixie, and the only
# prometheus packages there are Go, Rust, Perl and Poco ones (apt-cache pkgnames
# | grep -i prometheus).  There is nothing to adjust the name of.
#
# What GNUWorld's configure.ac looks for is the pkg-config pair
# "prometheus-cpp-core prometheus-cpp-pull", and upstream's own CMake build
# installs exactly those two files (cmake/prometheus-cpp-core.pc.in,
# cmake/prometheus-cpp-pull.pc.in), so nothing here writes a .pc file by hand.
#
# USE_THIRDPARTY_LIBRARIES=OFF: the pull component needs civetweb, which
# upstream carries as a git submodule and which a release tarball therefore does
# not contain - Debian's libcivetweb-dev is what it is built against instead.
# The "civetweb" binary package is installed with it because Debian's own CMake
# config for the library names /usr/bin/civetweb as an imported target, and
# find_package(civetweb) fails outright without that file.
# ENABLE_PUSH=OFF: GNUWorld uses core and pull only.
ARG PROMETHEUS_CPP_VERSION=1.3.0
RUN cd /tmp \
    && curl -sSL "https://codeload.github.com/jupp0r/prometheus-cpp/tar.gz/refs/tags/v${PROMETHEUS_CPP_VERSION}" \
        -o prometheus-cpp.tar.gz \
    && tar xzf prometheus-cpp.tar.gz \
    && cmake -S "prometheus-cpp-${PROMETHEUS_CPP_VERSION}" -B /tmp/prometheus-build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DBUILD_SHARED_LIBS=ON \
        -DENABLE_TESTING=OFF \
        -DENABLE_PUSH=OFF \
        -DENABLE_PULL=ON \
        -DUSE_THIRDPARTY_LIBRARIES=OFF \
    && cmake --build /tmp/prometheus-build --parallel "$(nproc)" \
    && cmake --install /tmp/prometheus-build \
    && ldconfig \
    && rm -rf /tmp/prometheus-cpp* /tmp/prometheus-build

# Where the two .pc files above landed, which is not on pkg-config's own path
ENV PKG_CONFIG_PATH=/usr/local/lib/pkgconfig

# Said here rather than found missing halfway through a build of GNUWorld: this
# image is worth nothing if these two are not where configure will look
RUN pkg-config --exists prometheus-cpp-core prometheus-cpp-pull \
    && printf 'prometheus-cpp-core %s, prometheus-cpp-pull %s\n' \
        "$(pkg-config --modversion prometheus-cpp-core)" \
        "$(pkg-config --modversion prometheus-cpp-pull)" \
    && pkg-config --exists libcurl \
    && printf 'libcurl %s\n' "$(pkg-config --modversion libcurl)"

# log4cplus is optional twice over: mod.dronescan uses it, and this image builds
# a second configuration with it when Debian has it.  Its absence is recorded
# rather than fatal, so that the image still builds if the package goes away.
RUN apt-get update \
    && { apt-get install -y --no-install-recommends liblog4cplus-dev \
         && echo yes > /usr/local/share/gnuworld-have-log4cplus ; } \
       || echo no > /usr/local/share/gnuworld-have-log4cplus \
    && rm -rf /var/lib/apt/lists/*

COPY build-and-test /usr/local/bin/build-and-test
COPY endpoint.py /usr/local/bin/gnuworld-pushover-endpoint
RUN chmod +x /usr/local/bin/build-and-test /usr/local/bin/gnuworld-pushover-endpoint

WORKDIR /src

# The tree arrives on stdin as a tar stream; build-and-test unpacks it here
ENTRYPOINT []
CMD ["/usr/local/bin/build-and-test"]
