# P11 capture stack (`test/harness/p11-env`)

Records real P11 server-to-server traffic from a live ircu hub, so it can be
replayed as fixtures by the pytest harness one directory up.

It runs a second, isolated copy of
[undernet-development-env](https://github.com/MrIron-no/undernet-development-env)
(hub, leaf and a gnuworld running `mod.debug` only) and is safe to run next to other ircu stacks on
the same host: it has its own compose project (`gnuworld-p11`), its own image
tags (`ircu2:p11-harness`, `gnuworld:p11-harness`) and its own host ports.

## Prerequisites

- Docker with Compose 2.24.4 or newer (the override uses `!override`).
- Checkouts next to this repository. Each path can be overridden from the
  environment, see the top of `run.sh`.

| Variable | Default | Notes |
|---|---|---|
| `DEVENV_DIR` | `../undernet-development-env` | only its compose file, Dockerfiles and `etc/` are used |
| `IRCU2_SRC` | `../ircu/ircu2-p11-harness` | **must be on `p11-integration`**; keep it a separate clone so nobody switches its branch under you |
| `IAUTHD_SRC` | `../ircu/iauthd-c` | |
| `GNUWORLD_SRC` | this repository | built inside Docker with `--enable-modules=debug`; host build artifacts are ignored |

The development environment's own `.env` is deliberately not read.

gnuworld is built and run with `mod.debug` only. The core is being overhauled
and `mod.debug` is the one module guaranteed to compile against it; it also has
no database, so Postgres is not started. `run.sh` derives the debug-only
Dockerfile and `gnuworld.conf` from the development environment's originals on
every run and writes them to `.generated/`, which git ignores.

## Host ports

| Service | Host port | Container port |
|---|---|---|
| leaf, clients | 16667 | 6667 |
| leaf, clients over TLS | 16697 | 6697 |
| hub, hidden client port | 16669 | 6669 |
| hub, plaintext server port | 14400 | 4400 |

## Recording a burst

A burst is only sent when a server links, so build the channel state first and
then make gnuworld relink.

```sh
./run.sh up                    # first run compiles ircu and the gnuworld core: slow
./scenario.py &                # builds the state and keeps its clients online
./run.sh restart gnuworld      # gnuworld relinks and receives the full burst
./run.sh logs gnuworld | grep -F '[OUT]'     # what gnuworld sent
cp capture/socket.log ../data/captures/<name>.log
./run.sh down
```

`capture/socket.log` holds the inbound stream, decrypted, one line per message,
each prefixed with a Unix timestamp. It is truncated on every gnuworld start,
unless `logrotate` is enabled in `gnuworld.conf`, in which case it is appended.
Outbound lines are only on gnuworld's stdout, tagged `[OUT]`.

`scenario.py` documents which channel exercises which part of the P11 BURST
layout. `OPLEVELS` is off in the stock development environment, so ops are
burst as `:o`; digit op levels need a capture with that feature enabled.
