"""A stealth module has no client on the network, and every xClient operation
has to be safe and correct for one.

Such a module does get a numeric (xNetwork::addClient) and is in localClients,
which is how nick@server routing reaches it, but it is never bursted: its
getInstance() is null and the uplink has never heard of its numeric. ircu
silently drops a line from an unknown numeric (ircd/parse.c), so a line written
from that numeric is not a message the network loses quietly - it is a bug.

What the core must do instead: what the network will not carry from a server
(JOIN, PART, SILENCE, WALLCHOPS, a NOTICE to $*) is not sent, and what it will
(WALLOPS, KILL) goes out from the server. Nothing may crash: a stealth module's
getInstance() is null, and Channel::findUser() asserts on a null client.
"""

from __future__ import annotations

import time

import pytest

import gnutest_client as gt
from p10 import p10_token, strip_msg_tags

CHAN = "#stealth"


async def run(hub, asker: str, command: str, timeout: float = 10.0) -> list[str]:
    """gnutest_client.run() for a stealth gnutest: it has no numeric to address,
    so it is reached as nick@server and answers from the server
    (test_messages.py::test_a_stealth_module_speaks_as_the_server)."""
    target = f"gnutest@{hub.peer_name}"
    after = len(hub.received)
    await hub.send_privmsg(asker, target, command)
    await hub.send_privmsg(asker, target, "sync")

    def is_sentinel(line: str) -> bool:
        return p10_token(line) == "O" and f" {asker} :" in line and gt.SENTINEL_REPLY in line

    await hub.wait_for(is_sentinel, timeout=timeout, after=after)

    lines = []
    for line in hub.received[after:]:
        if is_sentinel(line):
            break
        lines.append(strip_msg_tags(line))
    return lines


def alive(proc) -> None:
    assert proc.proc is not None and proc.proc.returncode is None, "gnuworld died"


def no_module_numeric(hub) -> None:
    """Nothing gnuworld wrote may be sourced from a numeric the uplink has never
    heard of. Both modules here are stealth and nothing else of ours is bursted,
    so any 5-character source under gnuworld's own YY is such a numeric."""
    for line in (strip_msg_tags(l) for l in hub.received):
        source = line.split(" ", 1)[0]
        assert not (len(source) == 5 and source.startswith(hub.peer_numeric)), (
            f"sourced from a numeric the uplink never heard of: {line}"
        )


async def setup(hub):
    """An asker, and CHAN as the hub's own channel."""
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.burst_channel(CHAN, members=[f"{asker}:o"], ts=int(time.time()) - 3600)
    return asker


@pytest.mark.asyncio
async def test_a_stealth_module_does_not_join(stealth_gnutest_linked_p11):
    """A server cannot JOIN or CREATE a channel, so nothing is sent. Before the
    guard in xServer::JoinChannel() this aborted on the assert in
    Channel::findUser() for a channel that exists, and dereferenced a null
    iClient after writing a bogus C line for one that does not."""
    hub, proc = stealth_gnutest_linked_p11
    asker = await setup(hub)

    for channel in (CHAN, "#stealth-new"):
        lines = await run(hub, asker, f"join {channel}")
        alive(proc)
        assert [l for l in lines if p10_token(l) in ("C", "J", "B")] == [], channel

    no_module_numeric(hub)


@pytest.mark.asyncio
async def test_a_stealth_module_does_not_part(stealth_gnutest_linked_p11):
    """It never joined, so there is nothing to leave and no server form of a
    PART. Before the guard this wrote a bogus L line and then aborted in
    xServer::OnPartChannel()."""
    hub, proc = stealth_gnutest_linked_p11
    asker = await setup(hub)

    lines = await run(hub, asker, f"part {CHAN}")
    alive(proc)
    assert [l for l in lines if p10_token(l) == "L"] == []

    no_module_numeric(hub)


@pytest.mark.asyncio
async def test_a_stealth_module_is_on_no_channel(stealth_gnutest_linked_p11):
    """It has no client on the network, so it is on no channel - not even one
    that exists. Before the null check in xClient::isOnChannel() this asked
    Channel::findUser() about a null client and aborted on its assert."""
    hub, proc = stealth_gnutest_linked_p11
    asker = await setup(hub)

    lines = await run(hub, asker, f"isonchannel {CHAN}")
    alive(proc)
    assert [l for l in lines if p10_token(l) == "O"] == [
        f"{hub.peer_numeric} O {asker} :{CHAN}: no"
    ]

    no_module_numeric(hub)


@pytest.mark.asyncio
async def test_a_stealth_module_wallops_and_kills_as_the_server(stealth_gnutest_linked_p11):
    """A server WALLOPS and a server KILL are both legal, so these are sent -
    from the server's numeric, not from the one the uplink has never heard of."""
    hub, proc = stealth_gnutest_linked_p11
    asker = await setup(hub)
    victim = await hub.introduce_nick("victim", username="victim")

    lines = await run(hub, asker, "wallops the roof is on fire")
    alive(proc)
    assert [l for l in lines if p10_token(l) == "WA"] == [
        f"{hub.peer_numeric} WA :the roof is on fire"
    ]

    lines = await run(hub, asker, "kill victim go away")
    alive(proc)
    assert [l for l in lines if p10_token(l) == "D"] == [
        f"{hub.peer_numeric} D {victim} gnutest :go away"
    ]

    no_module_numeric(hub)


@pytest.mark.asyncio
async def test_a_stealth_module_does_not_silence(stealth_gnutest_linked_p11):
    """ircu's ms_silence() answers a SILENCE from a server with a protocol
    violation, so there is no server form of it: nothing is sent."""
    hub, proc = stealth_gnutest_linked_p11
    asker = await setup(hub)
    await hub.introduce_nick("victim", username="victim")

    lines = await run(hub, asker, "silence victim *!*@spam.example")
    lines += await run(hub, asker, "unsilence *!*@spam.example")
    alive(proc)
    assert [l for l in lines if p10_token(l) == "U"] == []

    no_module_numeric(hub)


@pytest.mark.asyncio
async def test_a_stealth_module_notices_no_channel_ops(stealth_gnutest_linked_p11):
    """ircu's ms_wallchops() returns early on !IsUser(sptr) (ircd/m_wallchops.c),
    so a WALLCHOPS the server sends is delivered to nobody: nothing is sent, and
    it is not widened into a channel NOTICE that everybody would see."""
    hub, proc = stealth_gnutest_linked_p11
    asker = await setup(hub)

    lines = await run(hub, asker, f"noticechanops {CHAN} ops only")
    alive(proc)
    assert [l for l in lines if p10_token(l) in ("WC", "O")] == []

    no_module_numeric(hub)


@pytest.mark.asyncio
async def test_a_module_with_a_client_still_notices_channel_ops(gnutest_linked_p11):
    """The control case: with a client on the network the WALLCHOPS goes out."""
    hub, _proc = gnutest_linked_p11
    asker = await setup(hub)

    lines = await gt.run(hub, asker, f"noticechanops {CHAN} ops only")
    assert [l for l in lines if p10_token(l) == "WC"] == [
        f"{gt.numnick(hub)} WC {CHAN} :ops only"
    ]


@pytest.mark.asyncio
async def test_a_stealth_module_sends_no_global_notice(stealth_gnutest_linked_p11):
    """NOTICE $* needs an oper source: ircu's ms_notice() takes the $ branch
    only for IsOper(sptr), and a server is not an oper, so the notice would be
    dropped on arrival. gnuworld refuses it instead of claiming it was sent."""
    hub, proc = stealth_gnutest_linked_p11
    asker = await setup(hub)

    lines = await run(hub, asker, "globalnotice hello everybody")
    alive(proc)
    assert [l for l in lines if p10_token(l) == "O" and " $* :" in l] == []

    no_module_numeric(hub)
