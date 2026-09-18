"""SILENCE, the server's OPMODE on a user, a notice to everybody and a message
from the server: commands that more than one module sends, and so methods of
xClient and xServer. mod.gnutest calls them so that each is seen on the wire.

A command that only one module sends, that touches no state of ours and that
reads the same in P10 and P11, stays a Write() in that module: VERSION, TIME,
SETTIME and RPING of mod.ccontrol.
"""

from __future__ import annotations

import pytest

import gnutest_client as gt

PERMIT_ACCOUNT = "MrIron"


@pytest.mark.asyncio
async def test_commands_shared_between_modules(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, PERMIT_ACCOUNT)
    alice = await hub.introduce_nick("alice", username="alice")
    me, srv = gt.numnick(hub), hub.peer_numeric

    assert await gt.run(hub, asker, "silence alice *!*alice@*.example") == [
        f"{me} U {alice} *!*alice@*.example"
    ]
    assert await gt.run(hub, asker, "unsilence *!*alice@*.example") == [f"{me} U * -*!*alice@*.example"]

    assert await gt.run(hub, asker, "opmode alice +o") == [f"{srv} OM {alice} :+o"]
    assert await gt.run(hub, asker, "globalnotice back in five") == [f"{me} O $* :back in five"]
    assert await gt.run(hub, asker, "servsay alice hello") == [f"{srv} P {alice} :hello"]


@pytest.mark.asyncio
async def test_a_fake_client_parts_and_the_server_notices_a_channel(gnutest_linked_p11):
    """xServer::PartChannel() for a fake client, which mod.cloner did by hand,
    and serverNotice(), of which mod.openchanfix had a copy."""
    import time

    hub, _proc = gnutest_linked_p11
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, PERMIT_ACCOUNT)
    alice = await hub.introduce_nick("alice", username="alice")
    ts = int(time.time()) - 3600
    await hub.send_raw(f"{hub.server_numnick} B #parts {ts} +tn {alice}:o")

    introduced = await gt.run(hub, asker, "spawnclient fakey")
    fakey = next(line for line in introduced if " N fakey " in line).split(" :", 1)[0].split(" ")[-1]
    await gt.run(hub, asker, "spawnjoin fakey #parts")

    assert await gt.run(hub, asker, "servnotice #parts from the server") == [
        f"{hub.peer_numeric} O #parts :from the server"
    ]
    def parts(lines: list[str]) -> list[str]:
        return [line for line in lines if " L " in line]

    assert parts(await gt.run(hub, asker, "spawnpart fakey #parts")) == [f"{fakey} L #parts"]
    # Gone from our own view too: a second part finds nothing to part
    assert parts(await gt.run(hub, asker, "spawnpart fakey #parts")) == []


@pytest.mark.asyncio
async def test_shutdown_takes_our_clients_off_and_leaves(gnutest_linked_p11):
    """xServer::Shutdown(), which mod.nickserv replaced with a QUIT and a SQUIT
    of its own."""
    import asyncio

    hub, proc = gnutest_linked_p11
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, PERMIT_ACCOUNT)
    me = gt.numnick(hub)

    after = len(hub.received)
    await hub.send_privmsg(asker, me, "shutdown")
    await hub.wait_for(lambda line: line.startswith(f"{me} Q "), timeout=10.0, after=after)
    squit = await hub.wait_for(lambda line: " SQ " in line, timeout=10.0, after=after)
    # <YY> SQ <server> <link-ts> :<reason>, with 0 for whichever link it is
    assert squit.split(" :", 1)[0].endswith(f"{hub.peer_numeric} SQ {hub.peer_name} 0")
    assert proc.proc is not None
    assert await asyncio.wait_for(proc.proc.wait(), timeout=15) == 0


@pytest.mark.asyncio
async def test_lines_in_the_form_p11_documents(gnutest_linked_p11):
    """Found by reading what the core writes against ircu's doc/P11.md: ircu
    took each of these, but none was the form the protocol has."""
    hub, _proc = gnutest_linked_p11
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, PERMIT_ACCOUNT)
    me, srv = gt.numnick(hub), hub.peer_numeric

    # A user MODE names its target by nick; it was our own numnick
    assert await gt.run(hub, asker, "usermode +w") == [f"{me} M gnutest +w"]

    # A server we introduce: "+<flags>" where there was a bare 0
    (introduced,) = [l for l in await gt.run(hub, asker, "spawnserver spawned.testnet A spawned server")
                     if " S spawned.testnet " in l]
    fields = introduced.split(" :", 1)[0].split(" ")
    assert fields[:2] == [srv, "S"] and fields[-1] == "+" and fields[-3] == "J11"
