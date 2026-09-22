# GNUWorld integration tests (`test/harness`)

A pytest harness that tests gnuworld at the one seam it has: the
server-to-server protocol.

1. `fake_hub.py` listens on the loopback as an ircu hub, P10 or P11
2. gnuworld links to it, **run straight from the build tree**
3. Tests assert on the lines the hub receives, on gnuworld's stdout, and on
   gnuworld's own state as `mod.debug` reports it (`debugquery.py`);
   `mod.gnutest` calls the core API from chat commands, so a test can trigger
   an outbound operation and look at the line it produces (`gnutest_client.py`)

The C++ unit tests are separate: `make check`, from `test/*.cc`.

## Prerequisites

- A build of gnuworld: `./configure --enable-modules=debug,gnutest` and `make`.
  Nothing has to be installed. The suite runs the `gnuworld` wrapper at the top
  of the repository and loads the handlers and modules from there, so an edit
  needs `make` and nothing else before the tests see it.
- Python 3.11+ with `pytest`, `pytest-asyncio`, `pytest-timeout`.

Docker is only needed for tests that want Postgres, which today is
`mod.ccontrol`. A fixture whose module is not in the configured build skips,
saying so.

## Setup

```bash
cd test/harness
python3 -m venv .venv
source .venv/bin/activate
pip install pytest pytest-asyncio pytest-timeout
```

## Run

```bash
cd test/harness
pytest -q                          # about 15 s; no Docker involved
pytest -q test_p11_hidden.py       # one file
pytest -q --durations=10           # where the time goes
GNUWORLD_HARNESS=docker pytest -q  # the installed binaries, in the image
```

With `GNUWORLD_HARNESS=docker` gnuworld runs in a container built from the
installed tree (`bin/`, `lib/`, `share/gnuworld/`), so that mode does need
`make install` first. It is the way the harness always used to work, and is
kept as a check that the installed layout works too.

## Writing tests

```python
async def test_example(linked):
    hub, proc = linked
    await proc.wait_for_stdout("Connected")
    line = await hub.wait_for_token("EB")

async def test_check(ccontrol_linked):
    hub, proc = ccontrol_linked
    await hub.send_xquery(routing="iauth:1", message="CHECK ...")
```

## Logging

`test_logging.py` tests the hierarchical logger of `libgnuworld/logger.h`
end to end: the built-in default with no `logging.conf`, that a JSON sink's
lines all parse, an `irc` sink's highlighted notice, SIGHUP reopening a
renamed file and reloading a changed (or broken) `logging.conf`, per-module
`<module>.sql` on a forced SQL failure, and how cservice is configured — with
no `logger.cservice` line (its records go to the root's sinks and it writes
no log of its own), with the cservice section `bin/logging.example.conf`
ships (its own JSON file, the debug channel, the command log in the file
only), and with a `cservice.conf` that still carries the five logging keys
cservice no longer reads (one `WARN`, and nothing else changed). It is
written from `specs/2026-09-18-logger-core.md`, not from the implementation.

Two more cases are about what a sink sends rather than what it says: a
`cservice.conf` that still carries the four `pushover_*` keys (one `WARN`,
and neither the token nor the user key in any record or console line), and an
`irc` sink with `sink.<id>.rate = 2/min` (twenty records on one logger, all
twenty in the file sink beside it, at most two notices on the channel). The
flood for that one is twenty PRIVMSGs from a source numeric that does not
exist — one `WARN` on `core.proto` each — and deliberately not a SIGHUP:
reloading `logging.conf` builds a new sink, whose bucket starts full again.
The notice that says how many records were suppressed needs the bucket to
refill, half a minute away at that rate, so no harness test waits for it;
`test_logger_ratelimit` covers the counting behind it.

Note that the fake hub reads its socket only when it is asked to: count
notices after an `await hub.drain_messages(...)`, or `hub.received` will be
missing whatever arrived while nothing was waiting.

`link_cservice_logging` takes `cservice_extra=`, lines appended to
`cservice.conf` verbatim, for keys that are not in
`bin/cservice.example.conf` at all — `override_conf_keys` needs every key to
be there already, and the nine removed keys (five logging, four pushover) are
no longer there.

A test drops its own `logging.conf` into the conf dir before start by
passing `logging_conf=...` to `link_bare`, `link_cservice_logging` or
`link_ccontrol_logging` (`conftest.py`): the string is written to
`<conf_dir>/logging.conf` and a `logging_conf = <path>` key is appended to
`GNUWorld.conf`, so it is found the same way whether gnuworld runs locally
or, its conf dir bind-mounted read-only, in Docker. Cases that need the
local build's own command-line behaviour (no `-D`, an explicit `-d`/`-f`,
sending SIGHUP) run local-only and skip under `GNUWORLD_HARNESS=docker`.

Two fixtures mutate the shared Postgres database to force a failure and
always undo it, because the database is shared for the whole session:
`broken_webnotices_table` renames `webnotices` away so cservice's start-up
`DELETE FROM` it fails, and `blocked_user_commit` adds a trigger that
rejects an `UPDATE users` for a test nick. Both restore the schema (rename
the table back; drop the trigger and its function) in a `finally`, so a
failure inside the test does not leave the database changed for whatever
runs after it.

## Layout

| Path | Role |
|------|------|
| `docker-compose.yml` | Postgres + gnuworld image |
| `docker/Dockerfile` | Runtime image from host-built binaries |
| `docker/initdb/` | ccontrol schema for Postgres |
| `fake_hub.py` | Listening fake P10 hub |
| `gnuworld_proc.py` | `compose run` + stdout capture |
| `data/*.conf.in` | Config templates |

Gnuworld reaches FakeHub at `127.0.0.1:<port>` (container uses host networking).
ccontrol reaches Postgres at `127.0.0.1:<port>` too: neither port is a fixed
one, so that two checkouts can run the suite at the same time. Docker picks
Postgres's, and the harness writes it into the module configs it generates.

Each checkout also gets its own compose project, named after the tree, so two
runs never share a container or a volume either. To reach the database by hand:

```sh
cd test/harness
project=$(python3 -c 'import gnuworld_proc as g; print(g.COMPOSE_PROJECT)')
docker compose -p "$project" exec postgres psql -U gnuworld -d cservice
docker compose -p "$project" port postgres 5432   # that port, for a psql on the host
```
