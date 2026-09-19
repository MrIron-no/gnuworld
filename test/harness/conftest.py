"""Pytest fixtures for the GNUWorld fake-hub harness (Dockerized GW + Postgres)."""

from __future__ import annotations

import re
import asyncio
import os
import signal
import subprocess
from contextlib import asynccontextmanager
from pathlib import Path

import pytest
import pytest_asyncio

from fake_hub import FakeHub
from gnuworld_proc import (
    CONTAINER_UPLINK,
    DockerStack,
    GnuworldProc,
    LOCAL_BINARY,
    use_docker,
)


def built_modules() -> set[str] | None:
    """The modules this tree was configured with, or None if that is unknown.

    The image runs whatever is installed under lib/. A module that is not part
    of the current build may still be there from an older one, built against
    a core that has since changed, and then gnuworld does not even start.
    """
    config_status = Path(__file__).resolve().parents[2] / "config.status"
    try:
        text = config_status.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    match = re.search(r"--enable-modules=([A-Za-z0-9_,]+)", text)
    return set(match.group(1).split(",")) if match else None


def require_module(name: str) -> None:
    """Skip the calling test unless module ``name`` is part of the current build."""
    modules = built_modules()
    if modules is not None and name not in modules:
        pytest.skip(
            f"mod.{name} is not in this build (configured with "
            f"--enable-modules={','.join(sorted(modules))}); the copy under lib/ is stale"
        )


@pytest.fixture(scope="session")
def docker_stack():
    """The Docker side of the harness: the gnuworld image and Postgres.

    Nothing is built or started here. gnuworld normally runs from the build
    tree, which needs neither; a fixture that does need them calls ``up()``,
    which is mod.ccontrol for its database, and every fixture when
    GNUWORLD_HARNESS=docker. Most sessions therefore never touch Docker.
    """
    stack = DockerStack()
    if use_docker():
        stack.up()
    try:
        yield stack
    finally:
        if stack.started:
            stack.down()


@pytest_asyncio.fixture
async def fake_hub():
    """Listen on loopback; gnuworld uses host networking to reach it."""
    hub = FakeHub(host="127.0.0.1")
    await hub.start()
    try:
        yield hub
    finally:
        await hub.close()


@pytest_asyncio.fixture
async def fake_hub_p11():
    """Like fake_hub, but announces P11 (J11) in its SERVER line."""
    hub = FakeHub(host="127.0.0.1", protocol=11)
    await hub.start()
    try:
        yield hub
    finally:
        await hub.close()


def _prepare_conf_dir(tmp_path: Path) -> Path:
    """Host dir bind-mounted at /etc/gnuworld inside the container."""
    conf_dir = tmp_path / "etc-gnuworld"
    conf_dir.mkdir()
    return conf_dir


# --------------------------------------------------------------------------
# Logging test helpers (test_logging.py).  Additive only: nothing above this
# point is changed, and no existing fixture's behaviour changes.
# --------------------------------------------------------------------------


def override_conf_keys(path: Path, overrides: dict[str, str]) -> None:
    """Replaces ``key = ...`` lines of an already-written conf file in place.

    Same technique as GnuworldProc.write_module_config/write_cservice_config,
    for the confs those helpers do not take an override dict for (the main
    GNUWorld.conf, or a cservice.conf key not among write_cservice_config's
    own parameters). Every key must already appear exactly once.
    """
    text = path.read_text(encoding="utf-8")
    for key, value in overrides.items():
        text, count = re.subn(rf"(?m)^{re.escape(key)}\s*=.*$", f"{key} = {value}", text)
        assert count == 1, f"{key} not found once in {path}"
    path.write_text(text, encoding="utf-8")


class LocalGnuworldProc(GnuworldProc):
    """gnuworld started with the caller's own command-line flags, from the
    build tree only.

    The logging tests need combinations GnuworldProc.start() does not offer
    (no -D, an explicit -d, etc): what the harness's own fixtures fix at
    "-c ... -L -D" is exactly the command line these tests are about. Docker
    mode is not supported here - these cases are about the on-disk debug.log
    / logging.conf behaviour of a local checkout, not about the installed
    image - so a test using this skips under GNUWORLD_HARNESS=docker.
    """

    def __init__(self, conf_dir: Path, extra_args: list[str]):
        super().__init__(conf_dir=conf_dir, local=True)
        self.extra_args = extra_args

    async def start(self) -> None:
        if not (self.conf_dir / "GNUWorld.conf").is_file():
            raise FileNotFoundError(f"Missing {self.conf_dir / 'GNUWorld.conf'}")
        if not LOCAL_BINARY.is_file():
            raise FileNotFoundError(
                f"{LOCAL_BINARY} is not built: run make at the top of the repository"
            )
        cmd = [str(LOCAL_BINARY), "-f", str(self.conf_dir.resolve() / "GNUWorld.conf"), "-L",
               *self.extra_args]
        self.proc = await asyncio.create_subprocess_exec(
            *cmd,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
            cwd=str(self.conf_dir.resolve()),
            start_new_session=True,
        )
        self._reader_task = asyncio.create_task(self._read_stdout())


async def send_gnuworld_signal(proc: GnuworldProc, sig: int) -> None:
    """Sends a signal (SIGHUP for a reload, in practice) to a running
    GnuworldProc, in the build tree or in Docker."""
    if proc.local:
        os.killpg(proc.proc.pid, sig)
        return

    signame = signal.Signals(sig).name.removeprefix("SIG")
    await asyncio.to_thread(
        subprocess.run,
        ["docker", "exec", proc.container_name, "kill", f"-{signame}", "1"],
        capture_output=True,
        check=True,
    )


async def drop_connection(hub: FakeHub) -> None:
    """Closes the current TCP connection from gnuworld without tearing down
    the hub's listening socket (hub.close() does both), so that a second
    gnuworld connection - a reconnect - can still be accepted.

    Reaches into FakeHub's own connection state, which it does not expose a
    method for: accepting a second connection at all is not something any
    other test needs, only the IrcLogSink::forgetServer() reconnect case.
    """
    if hub._writer is not None:
        hub._writer.close()
        try:
            await hub._writer.wait_closed()
        except (OSError, ConnectionError):
            pass
    hub._reader = None
    hub._writer = None
    hub.connected = False
    hub.burst_complete = False
    hub.peer_name = None
    hub.peer_numeric = None


@asynccontextmanager
async def link_bare(
    docker_stack,
    hub: FakeHub,
    tmp_path: Path,
    *,
    logging_conf: str | None = None,
    auto_reconnect: bool = False,
    burst: list[str] | None = None,
):
    """gnuworld with no modules, against ``hub`` - like the ``gnuworld``
    fixture, but lets a test drop its own logging.conf before start (named by
    an absolute "logging_conf" key, so it is found whether gnuworld runs
    locally or, its conf dir bind-mounted, in Docker), turn auto-reconnect on,
    and give the hub's net burst (to create a channel before EB/EA, for an IRC
    sink to find). Yields (hub, proc, conf_dir).
    """
    conf_dir = _prepare_conf_dir(tmp_path)
    root = GnuworldProc.conf_root(conf_dir)
    GnuworldProc.write_config(
        conf_dir / "GNUWorld.conf", uplink=CONTAINER_UPLINK, port=hub.port, password=hub.password,
    )
    if logging_conf is not None:
        (conf_dir / "logging.conf").write_text(logging_conf, encoding="utf-8")
        with (conf_dir / "GNUWorld.conf").open("a", encoding="utf-8") as fh:
            fh.write(f"\nlogging_conf = {root}/logging.conf\n")
    if auto_reconnect:
        override_conf_keys(conf_dir / "GNUWorld.conf", {"auto_reconnect": "yes"})

    proc = GnuworldProc(conf_dir=conf_dir)
    await proc.start()
    try:
        await hub.accept_and_handshake(timeout=45.0, burst=burst)
        await proc.wait_for_stdout("Connected", timeout=30.0)
        yield hub, proc, conf_dir
    finally:
        await proc.terminate()


@asynccontextmanager
async def link_cservice_logging(
    docker_stack,
    hub: FakeHub,
    tmp_path: Path,
    *,
    burst: list[str] | None = None,
    logging_conf: str | None = None,
    cservice_overrides: dict[str, str] | None = None,
    cservice_extra: str | None = None,
):
    """gnuworld with mod.cservice (X) and stealth mod.debug on a P11 link, like
    cservice_linked, but lets a test supply the hub's net burst (to create a
    channel an irc sink of logging.conf names before it needs it), an optional
    logging.conf, extra cservice.conf key overrides, and extra cservice.conf
    lines.

    ``cservice_extra`` is appended to cservice.conf verbatim, for keys that are
    NOT in bin/cservice.example.conf at all - which override_conf_keys, needing
    every key to be there already, cannot add: the five logging keys cservice
    no longer reads are the case that wants it.

    Yields (hub, proc, conf_dir).
    """
    require_module("cservice")
    require_module("debug")
    docker_stack.up()
    conf_dir = _prepare_conf_dir(tmp_path)
    root = GnuworldProc.conf_root(conf_dir)
    GnuworldProc.write_cservice_config(conf_dir / "cservice.conf")
    if cservice_overrides:
        override_conf_keys(conf_dir / "cservice.conf", cservice_overrides)
    if cservice_extra:
        with (conf_dir / "cservice.conf").open("a", encoding="utf-8") as fh:
            fh.write("\n" + cservice_extra.rstrip("\n") + "\n")
    GnuworldProc.write_debug_config(conf_dir / "debug.conf")
    if logging_conf is not None:
        (conf_dir / "logging.conf").write_text(logging_conf, encoding="utf-8")
    GnuworldProc.write_config(
        conf_dir / "GNUWorld.conf",
        uplink=CONTAINER_UPLINK,
        port=hub.port,
        password=hub.password,
        module_lines=f"module = libcservice.la {root}/cservice.conf\n"
        f"module = libdebug.la {root}/debug.conf",
    )
    if logging_conf is not None:
        with (conf_dir / "GNUWorld.conf").open("a", encoding="utf-8") as fh:
            fh.write(f"\nlogging_conf = {root}/logging.conf\n")

    proc = GnuworldProc(conf_dir=conf_dir)
    await proc.start()
    try:
        await hub.accept_and_handshake(timeout=90.0, burst=burst)
        await proc.wait_for_stdout("Connected", timeout=60.0)
        assert hub.get_user_numnick("X"), "mod.cservice did not introduce X (module or database?)"
        yield hub, proc, conf_dir
    finally:
        await proc.terminate()


@asynccontextmanager
async def link_ccontrol_logging(
    docker_stack,
    hub: FakeHub,
    tmp_path: Path,
    *,
    logging_conf: str | None = None,
):
    """gnuworld with mod.ccontrol, like ccontrol_linked, but lets a test drop
    its own logging.conf before start. Local mode only (see link_bare):
    ccontrol_linked itself supports Docker, but the extra logging_conf plumbing
    here is only exercised locally by test_logging.py. Yields (hub, proc, conf_dir).
    """
    require_module("ccontrol")
    docker_stack.up()
    conf_dir = _prepare_conf_dir(tmp_path)
    root = GnuworldProc.conf_root(conf_dir)
    GnuworldProc.write_ccontrol_config(conf_dir / "ccontrol.conf")
    GnuworldProc.write_config(
        conf_dir / "GNUWorld.conf",
        uplink=CONTAINER_UPLINK,
        port=hub.port,
        password=hub.password,
        module_lines=f"module = libccontrol.la {root}/ccontrol.conf",
    )
    if logging_conf is not None:
        (conf_dir / "logging.conf").write_text(logging_conf, encoding="utf-8")
        with (conf_dir / "GNUWorld.conf").open("a", encoding="utf-8") as fh:
            fh.write(f"\nlogging_conf = {root}/logging.conf\n")

    proc = GnuworldProc(conf_dir=conf_dir)
    await proc.start()
    try:
        await hub.accept_and_handshake(timeout=90.0)
        await proc.wait_for_stdout("Connected", timeout=60.0)
        assert hub.get_user_numnick("euworld") or any(
            " N euworld " in line for line in hub.received
        ), "ccontrol did not burst euworld (module/DB load failed?)"
        yield hub, proc, conf_dir
    finally:
        await proc.terminate()


@pytest_asyncio.fixture
async def gnuworld(docker_stack, fake_hub, tmp_path):
    """Start Dockerized gnuworld (no modules) linked to FakeHub."""
    hub = fake_hub
    conf_dir = _prepare_conf_dir(tmp_path)
    GnuworldProc.write_config(
        conf_dir / "GNUWorld.conf",
        uplink=CONTAINER_UPLINK,
        port=hub.port,
        password=hub.password,
    )

    proc = GnuworldProc(conf_dir=conf_dir)
    await proc.start()
    try:
        await hub.accept_and_handshake(timeout=45.0)
        await proc.wait_for_stdout("Connected", timeout=30.0)
        yield hub, proc
    finally:
        await proc.terminate()


@pytest_asyncio.fixture
async def linked(gnuworld):
    yield gnuworld


@pytest_asyncio.fixture
async def ccontrol_linked(docker_stack, fake_hub, tmp_path):
    """gnuworld with libccontrol against the Postgres of the compose file,
    which publishes it on the host: gnuworld itself runs from the build tree,
    or in Docker under GNUWORLD_HARNESS=docker."""
    require_module("ccontrol")
    docker_stack.up()  # Postgres
    local = not use_docker()
    hub = fake_hub
    conf_dir = _prepare_conf_dir(tmp_path)
    GnuworldProc.write_ccontrol_config(conf_dir / "ccontrol.conf")
    GnuworldProc.write_config(
        conf_dir / "GNUWorld.conf",
        local=local,
        uplink=CONTAINER_UPLINK,
        port=hub.port,
        password=hub.password,
        module_lines=f"module = libccontrol.la {GnuworldProc.conf_root(conf_dir, local=local)}/ccontrol.conf",
    )

    proc = GnuworldProc(conf_dir=conf_dir, local=local)
    await proc.start()
    try:
        await hub.accept_and_handshake(timeout=90.0)
        await proc.wait_for_stdout("Connected", timeout=60.0)
        assert hub.get_user_numnick("euworld") or any(
            " N euworld " in line for line in hub.received
        ), "ccontrol did not burst euworld (module/DB load failed?)"
        yield hub, proc
    finally:
        await proc.terminate()


@pytest_asyncio.fixture
async def cservice_linked(docker_stack, fake_hub_p11, tmp_path):
    """gnuworld with mod.cservice (X) and stealth mod.debug on a P11 link,
    against the cservice database of the compose file's Postgres."""
    require_module("cservice")
    require_module("debug")
    docker_stack.up()  # Postgres
    hub = fake_hub_p11
    conf_dir = _prepare_conf_dir(tmp_path)
    root = GnuworldProc.conf_root(conf_dir)
    GnuworldProc.write_cservice_config(conf_dir / "cservice.conf")
    GnuworldProc.write_debug_config(conf_dir / "debug.conf")
    GnuworldProc.write_config(
        conf_dir / "GNUWorld.conf",
        uplink=CONTAINER_UPLINK,
        port=hub.port,
        password=hub.password,
        module_lines=f"module = libcservice.la {root}/cservice.conf\n"
        f"module = libdebug.la {root}/debug.conf",
    )

    proc = GnuworldProc(conf_dir=conf_dir)
    await proc.start()
    try:
        await hub.accept_and_handshake(timeout=90.0)
        await proc.wait_for_stdout("Connected", timeout=60.0)
        assert hub.get_user_numnick("X"), "mod.cservice did not introduce X (module or database?)"
        yield hub, proc
    finally:
        await proc.terminate()


@asynccontextmanager
async def link_module(docker_stack, hub, tmp_path, module: str, library: str, example: str,
                      settings: dict[str, str]):
    """gnuworld with one database module and stealth mod.debug, against the
    Postgres of the compose file. Yields (hub, proc)."""
    require_module(module)
    require_module("debug")
    docker_stack.up()  # Postgres
    conf_dir = _prepare_conf_dir(tmp_path)
    root = GnuworldProc.conf_root(conf_dir)
    GnuworldProc.write_module_config(conf_dir / f"{module}.conf", example, settings)
    GnuworldProc.write_debug_config(conf_dir / "debug.conf")
    GnuworldProc.write_config(
        conf_dir / "GNUWorld.conf",
        uplink=CONTAINER_UPLINK,
        port=hub.port,
        password=hub.password,
        module_lines=f"module = {library} {root}/{module}.conf\n"
        f"module = libdebug.la {root}/debug.conf",
    )
    proc = GnuworldProc(conf_dir=conf_dir)
    await proc.start()
    try:
        try:
            await hub.accept_and_handshake(timeout=90.0)
            await proc.wait_for_stdout("Connected", timeout=60.0)
        except Exception as error:
            # Say what gnuworld said last: a module that does not come up is
            # otherwise only seen as a link that closed
            await asyncio.sleep(3.0)  # let it finish saying why
            code = proc.proc.returncode if proc.proc else None
            tail = "\n".join(proc.stdout_lines[-25:])
            raise RuntimeError(f"mod.{module} did not link (gnuworld exit code {code}):\n{tail}") from error
        yield hub, proc
    finally:
        await proc.terminate()


_HARNESS_DB = {"host": "127.0.0.1", "port": "5433", "user": "gnuworld", "password": "gnuworld"}


@pytest_asyncio.fixture
async def nickserv_linked(docker_stack, fake_hub_p11, tmp_path):
    settings = {"dbHost": _HARNESS_DB["host"], "dbPort": _HARNESS_DB["port"], "dbDb": "nickserv",
                "dbUser": _HARNESS_DB["user"], "dbPass": _HARNESS_DB["password"]}
    async with link_module(docker_stack, fake_hub_p11, tmp_path, "nickserv", "libnickserv.la",
                           "nickserv.example.conf", settings) as linked:
        yield linked


@pytest_asyncio.fixture
async def dronescan_linked(docker_stack, fake_hub_p11, tmp_path):
    settings = {"sqlHost": _HARNESS_DB["host"], "sqlPort": _HARNESS_DB["port"], "sqlDB": "dronescan",
                "sqlUser": _HARNESS_DB["user"], "sqlPass": _HARNESS_DB["password"]}
    async with link_module(docker_stack, fake_hub_p11, tmp_path, "dronescan", "libdronescan.la",
                           "dronescan.example.conf", settings) as linked:
        yield linked


@pytest_asyncio.fixture
async def openchanfix_linked(docker_stack, fake_hub_p11, tmp_path):
    settings = {"sqlHost": _HARNESS_DB["host"], "sqlPort": _HARNESS_DB["port"], "sqlDB": "chanfix",
                "sqlcfUser": _HARNESS_DB["user"], "sqlPass": _HARNESS_DB["password"]}
    async with link_module(docker_stack, fake_hub_p11, tmp_path, "openchanfix", "libchanfix.la",
                           "openchanfix.example.conf", settings) as linked:
        yield linked


@pytest_asyncio.fixture
async def debug_linked(docker_stack, fake_hub, tmp_path):
    """Dockerized gnuworld with stealth mod.debug (no DB required)."""
    hub = fake_hub
    conf_dir = _prepare_conf_dir(tmp_path)
    GnuworldProc.write_debug_config(conf_dir / "debug.conf")
    GnuworldProc.write_config(
        conf_dir / "GNUWorld.conf",
        uplink=CONTAINER_UPLINK,
        port=hub.port,
        password=hub.password,
        module_lines=f"module = libdebug.la {GnuworldProc.conf_root(conf_dir)}/debug.conf",
    )

    proc = GnuworldProc(conf_dir=conf_dir)
    await proc.start()
    try:
        await hub.accept_and_handshake(timeout=90.0)
        await proc.wait_for_stdout("Connected", timeout=60.0)
        await proc.wait_for_stdout("Loaded stealth client, nickname: debug", timeout=30.0)
        yield hub, proc
    finally:
        await proc.terminate()


@asynccontextmanager
async def link_debug(
    hub: FakeHub,
    tmp_path: Path,
    burst: list[str] | None = None,
    *,
    gnutest: bool = False,
    burstchannel: str | None = None,
    gnutest_stealth: bool = False,
):
    """Run a Dockerized gnuworld with stealth mod.debug linked to ``hub``.

    ``burst`` is what the hub sends as its net burst (see fake_hub.load_capture).
    ``gnutest`` also loads mod.gnutest, which calls the core API from chat
    commands (see gnutest_client.py); ``burstchannel`` makes it claim a channel
    with xServer::BurstChannel() during gnuworld's own burst, and
    ``gnutest_stealth`` runs it with no client on the network (see test_stealth).
    gnuworld runs from the build tree unless GNUWORLD_HARNESS=docker, in which
    case the docker_stack fixture has to be active. Yields (hub, proc).
    """
    require_module("debug")
    if gnutest:
        require_module("gnutest")
    conf_dir = _prepare_conf_dir(tmp_path)
    GnuworldProc.write_debug_config(conf_dir / "debug.conf")
    modules = [f"module = libdebug.la {GnuworldProc.conf_root(conf_dir)}/debug.conf"]
    if gnutest:
        GnuworldProc.write_gnutest_config(conf_dir / "gnutest.conf", burstchannel=burstchannel,
                                          stealth=gnutest_stealth)
        modules.append(f"module = libgnutest.la {GnuworldProc.conf_root(conf_dir)}/gnutest.conf")
    GnuworldProc.write_config(
        conf_dir / "GNUWorld.conf",
        uplink=CONTAINER_UPLINK,
        port=hub.port,
        password=hub.password,
        module_lines="\n".join(modules),
    )

    proc = GnuworldProc(conf_dir=conf_dir)
    await proc.start()
    try:
        await hub.accept_and_handshake(timeout=90.0, burst=burst)
        await proc.wait_for_stdout("Connected", timeout=60.0)
        await proc.wait_for_stdout("Loaded stealth client, nickname: debug", timeout=30.0)
        yield hub, proc
    finally:
        await proc.terminate()


@pytest_asyncio.fixture
async def debug_linked_p11(docker_stack, fake_hub_p11, tmp_path):
    """Dockerized gnuworld with stealth mod.debug, linked to a P11 hub."""
    async with link_debug(fake_hub_p11, tmp_path) as linked:
        yield linked


@pytest_asyncio.fixture
async def gnutest_linked_p11(docker_stack, fake_hub_p11, tmp_path):
    """Dockerized gnuworld with mod.debug and mod.gnutest, linked to a P11 hub."""
    async with link_debug(fake_hub_p11, tmp_path, gnutest=True) as linked:
        yield linked


@pytest_asyncio.fixture
async def stealth_gnutest_linked_p11(docker_stack, fake_hub_p11, tmp_path):
    """Dockerized gnuworld with stealth mod.debug and stealth mod.gnutest, P11 hub."""
    async with link_debug(fake_hub_p11, tmp_path, gnutest=True, gnutest_stealth=True) as linked:
        yield linked


@pytest_asyncio.fixture
async def gnutest_linked_p10(docker_stack, fake_hub, tmp_path):
    """Dockerized gnuworld with mod.debug and mod.gnutest, linked to a P10 hub."""
    async with link_debug(fake_hub, tmp_path, gnutest=True) as linked:
        yield linked


@pytest_asyncio.fixture
async def debug_linked_tls(docker_stack, tmp_path):
    """Stealth mod.debug over a TLS uplink (hub SERVER flags omit +z)."""
    conf_dir = _prepare_conf_dir(tmp_path)
    hub_crt, hub_key, _gw_crt, _gw_key = GnuworldProc.install_tls_certs(conf_dir)

    hub = FakeHub(
        host="127.0.0.1",
        tls=True,
        tls_certfile=hub_crt,
        tls_keyfile=hub_key,
        # Intentionally no 'z' — tls=yes must come from the transport.
        server_flags="hs",
    )
    await hub.start()

    GnuworldProc.write_debug_config(conf_dir / "debug.conf")
    GnuworldProc.write_config(
        conf_dir / "GNUWorld.conf",
        uplink=CONTAINER_UPLINK,
        port=hub.port,
        password=hub.password,
        module_lines=f"module = libdebug.la {GnuworldProc.conf_root(conf_dir)}/debug.conf",
        tls=True,
    )

    proc = GnuworldProc(conf_dir=conf_dir)
    await proc.start()
    try:
        await hub.accept_and_handshake(timeout=90.0)
        await proc.wait_for_stdout("Connected", timeout=60.0)
        await proc.wait_for_stdout("TLS handshake completed successfully", timeout=30.0)
        await proc.wait_for_stdout("Loaded stealth client, nickname: debug", timeout=30.0)
        yield hub, proc
    finally:
        await proc.terminate()
        await hub.close()
