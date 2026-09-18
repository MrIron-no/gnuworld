"""mod.cloner keeps a list of its clones. A clone that the network kills is
deleted by the core; the module did not listen for the kill, kept the pointer,
and the next SAYALL wrote through it."""

from __future__ import annotations

import asyncio
import re
import time

import pytest
import pytest_asyncio

from conftest import CONTAINER_UPLINK, _prepare_conf_dir, require_module
from gnuworld_proc import GnuworldProc, REPO_ROOT
from p10 import p10_token, strip_msg_tags


@pytest_asyncio.fixture
async def cloner_linked(docker_stack, fake_hub_p11, tmp_path):
    require_module("cloner")
    hub = fake_hub_p11
    conf_dir = _prepare_conf_dir(tmp_path)
    example = (REPO_ROOT / "bin" / "cloner.example.conf").read_text()
    (conf_dir / "cloner.conf").write_text(re.sub(r"(?m)^cloneburstcount\s*=.*$", "cloneburstcount = 10", example))
    GnuworldProc.write_config(
        conf_dir / "GNUWorld.conf",
        uplink=CONTAINER_UPLINK,
        port=hub.port,
        password=hub.password,
        module_lines=f"module = libcloner.la {GnuworldProc.conf_root(conf_dir)}/cloner.conf",
    )
    proc = GnuworldProc(conf_dir=conf_dir)
    await proc.start()
    try:
        await hub.accept_and_handshake(timeout=90.0)
        await proc.wait_for_stdout("Connected", timeout=60.0)
        yield hub, proc
    finally:
        await proc.terminate()


@pytest.mark.asyncio
async def test_a_killed_clone_is_forgotten(cloner_linked):
    hub, proc = cloner_linked
    oper = await hub.introduce_nick("operone", username="operone", modes="+io")
    alice = await hub.introduce_nick("alice", username="alice")
    ts = int(time.time()) - 3600
    await hub.send_raw(f"{hub.server_numnick} B #clones {ts} +tn {alice}:o")

    cloner = hub.get_user_numnick("Cloner")
    assert cloner, "gnuworld did not introduce the cloner"

    # Two clones, which the module makes on its timer
    after = len(hub.received)
    await hub.send_privmsg(oper, cloner, "loadclones 2")
    clones: list[str] = []

    def is_clone(line: str) -> bool:
        # The hub reads the link while it waits; "N <nick> 2 ..." is a client
        # on the spawned server, one hop behind gnuworld
        if p10_token(line) == "N" and strip_msg_tags(line).split(" ")[0] != hub.peer_numeric:
            clones.append(strip_msg_tags(line).split(" :", 1)[0].split(" ")[-1])
        return len(clones) == 2

    await hub.wait_for(is_clone, timeout=20.0, after=after)

    # The network kills the first
    await hub.send_raw(f"{hub.server_numnick} D {clones[0]} hub.testnet!oper :(bye)")

    after = len(hub.received)
    await hub.send_privmsg(oper, cloner, "sayall #clones still here")
    speakers: list[str] = []

    def spoke(line: str) -> bool:
        if p10_token(line) == "P" and "still here" in line:
            speakers.append(strip_msg_tags(line).split(" ")[0])
        return False

    # Read what there is for a moment: a second speaker would show up here
    with pytest.raises((TimeoutError, asyncio.TimeoutError)):
        await hub.wait_for(spoke, timeout=1.5, after=after)
    assert speakers == [clones[1]], speakers
    assert proc.proc is not None and proc.proc.returncode is None
