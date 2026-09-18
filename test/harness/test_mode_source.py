"""Who a channel change comes from: the xClient itself, or the server.

Every mode helper of xClient takes a SendAs. As the client it has to be on the
channel with ops, and joins and parts around the change if it is not there. As
the server it needs neither, and the network applies the change regardless.
"""

from __future__ import annotations

import time

import pytest

import gnutest_client as gt
from debugquery import chaninfo

PERMIT_ACCOUNT = "MrIron"
CHAN = "#source"
BOBMASK = "*!bob@bob.example.net"


async def _setup(hub) -> dict:
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, PERMIT_ACCOUNT)
    n = {
        nick: await hub.introduce_nick(nick, username=nick, host=f"{nick}.example.net")
        for nick in ("alice", "bob", "carol")
    }
    ts = int(time.time()) - 3600
    await hub.send_raw(f"{hub.server_numnick} B {CHAN} {ts} +tn {n['bob']},{n['carol']},{n['alice']}:o")
    return dict(n, asker=asker, ts=ts, me=gt.numnick(hub), srv=hub.peer_numeric, c=CHAN)


@pytest.mark.asyncio
async def test_as_the_server_without_being_on_the_channel(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    v = await _setup(hub)

    async def run(command: str) -> list[str]:
        return await gt.run(hub, v["asker"], command.format(**v))

    def lines(*expected: str) -> list[str]:
        return [line.format(**v) for line in expected]

    # One line each, from the server: no join, no part
    assert await run("servop {c} bob") == lines("{srv} M {c} +o {bob} {ts}")
    assert await run("servvoice {c} bob carol") == lines("{srv} M {c} +vv {bob} {carol} {ts}")
    assert await run("servdevoice {c} carol") == lines("{srv} M {c} -v {carol} {ts}")
    assert await run("servdeop {c} bob alice") == lines("{srv} M {c} -oo {bob} {alice} {ts}")
    assert await run("servban {c} bob") == lines("{srv} M {c} +b " + BOBMASK + " {ts}")
    assert await run("servunban {c} " + BOBMASK) == lines("{srv} M {c} -b " + BOBMASK + " {ts}")
    assert await run("servbanmask {c} *!*@one.example *!*@two.example") == lines(
        "{srv} M {c} +bb *!*@one.example *!*@two.example {ts}"
    )
    assert await run("servmode {c} +mov bob carol") == lines("{srv} M {c} +mov {bob} {carol} {ts}")

    info = await chaninfo(hub, v["asker"], CHAN)
    assert info.members == {"alice": "none", "bob": "+o+v", "carol": "+v"}  # gnutest never joined
    assert info.bans == {"*!*@one.example", "*!*@two.example"}
    assert info.modes == "mnt"


@pytest.mark.asyncio
async def test_as_the_client_joins_and_parts_around_the_change(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    v = await _setup(hub)

    async def run(command: str) -> list[str]:
        return await gt.run(hub, v["asker"], command.format(**v))

    for command, change in (
        ("voice {c} bob carol", "+vv {bob} {carol}"),
        ("ban {c} bob", "+b " + BOBMASK),
        ("unban {c} " + BOBMASK, "-b " + BOBMASK),
        ("mode {c} +m", "+m"),
    ):
        assert await run(command) == [
            line.format(**v)
            for line in (
                "{me} J {c} {ts}",
                "{srv} M {c} +o {me} {ts}",
                "{me} M {c} " + change + " {ts}",
                "{me} L {c} :",
            )
        ], command

    # Nothing to change means nothing is sent, not even the join
    assert await run("voice {c} bob") == []
    assert await run("unban {c} *!*@nobody.example *!*@nobody2.example") == []

    assert "gnutest" not in (await chaninfo(hub, v["asker"], CHAN)).members


@pytest.mark.asyncio
async def test_as_the_client_on_the_channel_without_ops_fails(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    v = await _setup(hub)

    async def run(command: str) -> list[str]:
        return await gt.run(hub, v["asker"], command.format(**v))

    assert await run("join {c}") == [f"{v['me']} J {CHAN} {v['ts']}"]

    # On the channel, not opped: the network would bounce it, so it is not sent
    for command in ("op {c} bob", "voice {c} bob carol", "ban {c} bob", "mode {c} +m",
                    "banmask {c} *!*@one.example"):
        assert await run(command) == [], command

    info = await chaninfo(hub, v["asker"], CHAN)
    assert info.members == {"alice": "+o", "bob": "none", "carol": "none", "gnutest": "none"}
    assert (info.bans, info.modes) == (set(), "nt")

    # The server can still do it
    assert await run("servop {c} bob") == [f"{v['srv']} M {CHAN} +o {v['bob']} {v['ts']}"]
    assert (await chaninfo(hub, v["asker"], CHAN)).members["bob"] == "+o"


@pytest.mark.asyncio
async def test_a_network_service_is_not_deopped_or_banned(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    v = await _setup(hub)
    service = await hub.introduce_nick("service", username="service", modes="+ik")
    await hub.send_raw(f"{service} J {CHAN} {v['ts']}")
    await hub.send_raw(f"{hub.server_numnick} M {CHAN} +o {service} {v['ts']}")

    async def run(command: str) -> list[str]:
        return await gt.run(hub, v["asker"], command.format(**v))

    assert await run("servdeop {c} service") == []
    assert await run("servban {c} service") == []
    # Among several targets it is skipped and the others go through
    assert await run("servdeop {c} service alice") == [
        f"{v['srv']} M {CHAN} -o {v['alice']} {v['ts']}"
    ]
    assert (await chaninfo(hub, v["asker"], CHAN)).members["service"] == "+o"


@pytest.mark.asyncio
async def test_a_mode_with_an_older_timestamp_makes_the_channel_older(gnutest_linked_p11):
    """Every MODE line ends in the channel's creation time, and a server that is
    told an older one takes it over (ircu, mode_parse()). mod.cservice sets +R
    on a registered channel with the time the channel was registered with;
    what ircu then believes about the channel, gnuworld has to believe too."""
    from debugquery import chaninfo

    hub, _proc = gnutest_linked_p11
    v = await _setup(hub)
    srv, ts = v["srv"], v["ts"]

    async def run(command: str) -> list[str]:
        return await gt.run(hub, v["asker"], command.format(**v))

    older = ts - 5000000
    assert await run(f"servmodets {{c}} {older} +R") == [f"{srv} M {CHAN} +R {older}"]
    assert (await chaninfo(hub, v["asker"], CHAN)).created == older

    # A younger one would have ircu bounce the line: ours stands
    assert await run(f"servmodets {{c}} {ts} +m") == [f"{srv} M {CHAN} +m {older}"]
    assert (await chaninfo(hub, v["asker"], CHAN)).created == older

    # Nothing to change, nothing sent, and so nothing taken over either
    assert await run(f"servmodets {{c}} {older - 100} -b *!*@nobody.example") == []
    assert (await chaninfo(hub, v["asker"], CHAN)).created == older

    # A number among the arguments is an argument, or a mistake; never a time
    assert await run("servmode {c} +l 25") == [f"{srv} M {CHAN} +l 25 {older}"]
    assert await run(f"servmode {{c}} +i {older - 100}") == []
    assert (await chaninfo(hub, v["asker"], CHAN)).created == older
