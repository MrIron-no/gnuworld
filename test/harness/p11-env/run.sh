#!/bin/sh
# Wrapper around `docker compose` for the P11 capture stack.
#   ./run.sh up        build and start hub, leaf and gnuworld (mod.debug only)
#   ./run.sh logs -f gnuworld
#   ./run.sh down      stop and remove containers
# Any other arguments are passed straight to `docker compose`.
#
# Paths can be overridden from the environment.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
gnuworld_root=$(cd "$here/../../.." && pwd)

: "${DEVENV_DIR:=$gnuworld_root/../undernet-development-env}"
: "${IRCU2_SRC:=$gnuworld_root/../ircu/ircu2-p11-harness}"   # must be on p11-integration
: "${IAUTHD_SRC:=$gnuworld_root/../ircu/iauthd-c}"
: "${GNUWORLD_SRC:=$gnuworld_root}"
: "${P11_CAPTURE_DIR:=$here/capture}"
P11_GEN_DIR=$here/.generated
export IRCU2_SRC IAUTHD_SRC GNUWORLD_SRC P11_CAPTURE_DIR P11_GEN_DIR

for p in "$DEVENV_DIR/docker-compose.yml" "$IRCU2_SRC/ircd" "$IAUTHD_SRC" "$GNUWORLD_SRC/src"; do
  [ -e "$p" ] || { echo "run.sh: missing $p" >&2; exit 1; }
done

# Derive the debug-only build and config from the development environment's
# own files on every run, so they cannot drift and so its link password is
# never copied into this repository (.generated/ is ignored by git).
generate() {
  src_etc=$DEVENV_DIR/etc/gnuworld
  mkdir -p "$P11_GEN_DIR/etc/certs"

  # Own BuildKit cache ids: the configure arguments differ from the
  # development environment's, and sharing its cache would force a full
  # reconfigure on both sides every time either one builds.
  sed -E \
    -e 's/--enable-modules=[A-Za-z0-9_,]+/--enable-modules=debug/' \
    -e 's/id=undernet-gnuworld-(work|ccache)-[A-Za-z0-9]+/id=gnuworld-p11-\1-v1/g' \
    "$DEVENV_DIR/Dockerfile.gnuworld" > "$P11_GEN_DIR/Dockerfile.gnuworld"
  grep -q -- '--enable-modules=debug ' "$P11_GEN_DIR/Dockerfile.gnuworld" ||
    { echo "run.sh: could not rewrite --enable-modules in Dockerfile.gnuworld" >&2; exit 1; }

  { grep -vE '^[[:space:]]*module[[:space:]]*=' "$src_etc/gnuworld.conf"
    echo 'module = libdebug.la /gnuworld/etc/debug.conf'
  } > "$P11_GEN_DIR/etc/gnuworld.conf"
  cp "$src_etc/debug.conf" "$src_etc/logging.properties" "$P11_GEN_DIR/etc/"
}

compose() {
  # --env-file /dev/null: do not inherit the development environment's own
  # .env, which may point IRCU2_SRC at a checkout on some other branch.
  docker compose -p gnuworld-p11 --env-file /dev/null \
    -f "$DEVENV_DIR/docker-compose.yml" -f "$here/compose.p11.yml" "$@"
}

generate
case "${1:-}" in
  up)  shift; compose up -d --build "$@" hub leaf gnuworld ;;
  *)   compose "$@" ;;
esac
