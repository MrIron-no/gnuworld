"""What mod.cservice (X) sends for the commands that change a channel, and what
gnuworld then believes about the channel.

The lines were recorded from X as it was before it was moved to the core API,
when it wrote some of them itself and kept the channel's state by hand. They
are the same now, but for what was wrong then: MODE +b and -b sent nothing, as
X had put the ban on the channel's list before asking for it, and TOPIC went
out without the channel's timestamp.
"""

from __future__ import annotations

import itertools
import os
import time

import pytest

import cservice_client as cs
from debugquery import chaninfo
from p10 import p10_token

DEBUG_ACCOUNT = "MrIron"

_channels = itertools.count(1)


async def _setup(hub):
    # The database outlives a test, and a channel is registered once: a name
    # of its own for every test
    chan = f"#reg{os.getpid()}x{next(_channels)}"
    admin = await cs.login(hub)
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, DEBUG_ACCOUNT)
    n = {nick: await hub.introduce_nick(nick, username=nick, host=f"{nick}.example.net")
         for nick in ("alice", "bob")}
    ts = int(time.time()) - 3600
    await hub.send_raw(f"{hub.server_numnick} B {chan} {ts} +tn {admin},{n['bob']},{n['alice']}:o")

    x, srv = cs.numnick(hub), hub.peer_numeric
    registered = cs.network(await cs.run(hub, admin, f"register {chan} Admin"), admin)
    assert registered[:3] == [
        f"{x} J {chan} {ts}",
        f"{srv} M {chan} +o {x} {ts}",
        f"{x} M {chan} +tnR {ts}",
    ], registered
    return chan, admin, asker, n, ts


@pytest.mark.asyncio
async def test_mode_command(cservice_linked):
    hub, _proc = cservice_linked
    CHAN, admin, asker, n, ts = await _setup(hub)
    x = cs.numnick(hub)

    async def sent(command: str) -> list[str]:
        return cs.network(await cs.run(hub, admin, command), admin)

    assert await sent(f"mode {CHAN} +im") == [f"{x} M {CHAN} +im {ts}"]
    assert await sent(f"mode {CHAN} +k sekrit") == [f"{x} M {CHAN} +k sekrit {ts}"]
    assert await sent(f"mode {CHAN} +l 25") == [f"{x} M {CHAN} +l 25 {ts}"]
    assert await sent(f"mode {CHAN} +b *!*@spam.example") == [f"{x} M {CHAN} +b *!*@spam.example {ts}"]

    info = await chaninfo(hub, asker, CHAN)
    assert info.modes == "".join(sorted("Riklmnt"))
    assert (info.key, info.limit) == ("sekrit", 25)
    assert info.bans == {"*!*@spam.example"}

    # The key has to be named to take it off
    assert await sent(f"mode {CHAN} -k wrong") == []
    assert await sent(f"mode {CHAN} -k sekrit") == [f"{x} M {CHAN} -k sekrit {ts}"]
    assert await sent(f"mode {CHAN} -b *!*@spam.example") == [f"{x} M {CHAN} -b *!*@spam.example {ts}"]
    info = await chaninfo(hub, asker, CHAN)
    assert (info.key, info.bans) == (None, set())


@pytest.mark.asyncio
async def test_mode_command_lets_a_user_change_only_what_x_allows(cservice_linked):
    """The whitelist is X's: the core would set or clear any of these."""
    hub, _proc = cservice_linked
    CHAN, admin, asker, _n, _ts = await _setup(hub)
    before = await chaninfo(hub, asker, CHAN)

    for command in (f"mode {CHAN} -R", f"mode {CHAN} +o bob", f"mode {CHAN} +v bob",
                    f"mode {CHAN} +A apass", f"mode {CHAN} +z"):
        assert cs.network(await cs.run(hub, admin, command), admin) == [], command

    after = await chaninfo(hub, asker, CHAN)
    assert (after.modes, after.members) == (before.modes, before.members)


@pytest.mark.asyncio
async def test_mode_command_changes_nothing_when_x_cannot(cservice_linked):
    """X on the channel without ops: nothing is sent. X used to have changed the
    channel's state by hand before it asked, so gnuworld believed in modes the
    network never saw."""
    hub, _proc = cservice_linked
    CHAN, admin, asker, _n, ts = await _setup(hub)
    x = cs.numnick(hub)

    await hub.send_raw(f"{hub.server_numnick} M {CHAN} -o {x} {ts}")
    sent = cs.network(await cs.run(hub, admin, f"mode {CHAN} +im"), admin)
    assert [line for line in sent if f" M {CHAN} +im" in line] == []
    assert "i" not in (await chaninfo(hub, asker, CHAN)).modes


@pytest.mark.asyncio
async def test_member_commands(cservice_linked):
    hub, _proc = cservice_linked
    CHAN, admin, asker, n, ts = await _setup(hub)
    x = cs.numnick(hub)

    async def modes_sent(command: str) -> list[str]:
        return [l for l in await cs.run(hub, admin, command) if p10_token(l) in ("M", "K", "T", "I", "CM")]

    assert await modes_sent(f"op {CHAN} bob") == [f"{x} M {CHAN} +o {n['bob']} {ts}"]
    assert await modes_sent(f"voice {CHAN} bob") == [f"{x} M {CHAN} +v {n['bob']} {ts}"]
    assert (await chaninfo(hub, asker, CHAN)).members["bob"] == "+o+v"
    assert await modes_sent(f"devoice {CHAN} bob") == [f"{x} M {CHAN} -v {n['bob']} {ts}"]
    assert await modes_sent(f"deop {CHAN} bob") == [f"{x} M {CHAN} -o {n['bob']} {ts}"]
    assert await modes_sent(f"invite {CHAN}") == [f"{x} I {admin} {CHAN} {ts}"]

    # A ban on a nick: the mask on the channel, then the kick
    assert await modes_sent(f"ban {CHAN} bob 1h 75 go away") == [
        f"{x} M {CHAN} +b *!bob@bob.example.net {ts}",
        f"{x} K {CHAN} {n['bob']} :(Admin) go away",
    ]
    info = await chaninfo(hub, asker, CHAN)
    assert info.bans == {"*!bob@bob.example.net"} and "bob" not in info.members


@pytest.mark.asyncio
async def test_topic_carries_the_channels_timestamp_and_is_recorded(cservice_linked):
    hub, _proc = cservice_linked
    CHAN, admin, asker, _n, ts = await _setup(hub)
    x = cs.numnick(hub)

    (line,) = [l for l in await cs.run(hub, admin, f"topic {CHAN} hello there") if p10_token(l) == "T"]
    fields, text = line.split(" :", 1)
    parts = fields.split(" ")
    assert parts[:4] == [x, "T", CHAN, str(ts)] and abs(int(parts[4]) - time.time()) < 300
    assert text == "(Admin) hello there"
    assert (await chaninfo(hub, asker, CHAN)).topic == "(Admin) hello there"


@pytest.mark.asyncio
async def test_x_is_opped_again_by_the_server_and_gnuworld_knows_it(cservice_linked):
    """The reop: "<server> M #chan +o X", which X wrote itself and then set its
    own op bit for. xServer::Op() now."""
    hub, _proc = cservice_linked
    CHAN, admin, asker, _n, ts = await _setup(hub)
    x, srv = cs.numnick(hub), hub.peer_numeric

    after = len(hub.received)
    await hub.send_raw(f"{hub.server_numnick} M {CHAN} -o {x} {ts}")
    reop = await hub.wait_for(lambda l: f"{srv} M {CHAN} +o {x}" in l, timeout=30.0, after=after)
    assert reop.endswith(f"{srv} M {CHAN} +o {x} {ts}")
    assert (await chaninfo(hub, asker, CHAN)).members["X"] == "+o"


@pytest.mark.asyncio
async def test_purge(cservice_linked):
    hub, _proc = cservice_linked
    CHAN, admin, asker, _n, ts = await _setup(hub)
    x, srv = cs.numnick(hub), hub.peer_numeric

    sent = cs.network(await cs.run(hub, admin, f"purge {CHAN} testing"), admin)
    assert f"{srv} M {CHAN} -R {ts}" in sent
    assert sent[-1] == f"{x} L {CHAN} :"
    info = await chaninfo(hub, asker, CHAN)
    assert "R" not in info.modes and "X" not in info.members
