"""Pytest fixtures for the GNUWorld fake-hub harness (Dockerized GW + Postgres)."""

from __future__ import annotations

import re
from contextlib import asynccontextmanager
from pathlib import Path

import pytest
import pytest_asyncio

from fake_hub import FakeHub
from gnuworld_proc import (
    CONTAINER_UPLINK,
    DockerStack,
    GnuworldProc,
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
):
    """Run a Dockerized gnuworld with stealth mod.debug linked to ``hub``.

    ``burst`` is what the hub sends as its net burst (see fake_hub.load_capture).
    ``gnutest`` also loads mod.gnutest, which calls the core API from chat
    commands (see gnutest_client.py); ``burstchannel`` makes it claim a channel
    with xServer::BurstChannel() during gnuworld's own burst.
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
        GnuworldProc.write_gnutest_config(conf_dir / "gnutest.conf", burstchannel=burstchannel)
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
