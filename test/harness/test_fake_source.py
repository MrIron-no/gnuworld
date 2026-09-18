"""Channel changes made by a fake client.

It exists only as an iClient, which has no methods to call. The server-side
API therefore takes it as its last argument, where null is the server itself:

    MyUplink->Op(theChan, target, fakeClient);

A fake client is held to what the network would hold it to: it has to be on the
channel, opped. The server needs neither.
"""

from __future__ import annotations

import time

import pytest

import gnutest_client as gt
from debugquery import chaninfo
from p10 import p10_token, strip_msg_tags

PERMIT_ACCOUNT = "MrIron"
CHAN = "#fakes"


async def _setup(hub) -> dict:
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, PERMIT_ACCOUNT)
    n = {nick: await hub.introduce_nick(nick, username=nick) for nick in ("alice", "bob", "carol")}
    ts = int(time.time()) - 3600
    await hub.send_raw(f"{hub.server_numnick} B {CHAN} {ts} +tn {n['bob']},{n['carol']},{n['alice']}:o")
    return dict(n, asker=asker, ts=ts, srv=hub.peer_numeric, c=CHAN)


def _introduced(lines: list[str], token: str) -> str:
    """The numeric of the client (N) or server (S) introduced in these lines."""
    for line in lines:
        if p10_token(line) == token:
            fields = strip_msg_tags(line).split(" :", 1)[0].split(" ")
            # N: the numnick is the last field; S: "<name> <hops> <boot> <link> <proto> <YYXXX> ..."
            return fields[-1] if token == "N" else fields[7][:2]
    raise AssertionError(f"no {token} in {lines}")


@pytest.mark.asyncio
async def test_a_fake_client_changes_a_channel_it_is_opped_on(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    v = await _setup(hub)

    async def run(command: str) -> list[str]:
        return await gt.run(hub, v["asker"], command.format(**v))

    v["fake"] = _introduced(await run("spawnclient fakey"), "N")
    await run("spawnjoin fakey {c}")

    # On the channel but not opped: the network would bounce it, so nothing is sent
    assert await run("as fakey op {c} bob") == []
    assert await run("as fakey mode {c} +m") == []

    await hub.send_raw(f"{hub.server_numnick} M {CHAN} +o {v['fake']} {v['ts']}")

    assert await run("as fakey op {c} bob") == [f"{v['fake']} M {CHAN} +o {v['bob']} {v['ts']}"]
    assert await run("as fakey voice {c} bob carol") == [
        f"{v['fake']} M {CHAN} +vv {v['bob']} {v['carol']} {v['ts']}"
    ]
    assert await run("as fakey banmask {c} *!*@one.example") == [
        f"{v['fake']} M {CHAN} +b *!*@one.example {v['ts']}"
    ]
    assert await run("as fakey mode {c} +ml 10") == [f"{v['fake']} M {CHAN} +ml 10 {v['ts']}"]
    # Not an oper, so a ClearMode goes out as plain modes, with their arguments
    assert await run("as fakey clearmode {c} lb") == [
        f"{v['fake']} M {CHAN} -lb *!*@one.example {v['ts']}"
    ]
    info = await chaninfo(hub, v["asker"], CHAN)
    assert info.members == {"alice": "+o", "bob": "+o+v", "carol": "+v", "fakey": "+o"}
    assert (info.modes, info.limit, info.bans) == ("mnt", None, set())


@pytest.mark.asyncio
async def test_a_fake_client_that_is_not_on_the_channel_cannot(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    v = await _setup(hub)

    async def run(command: str) -> list[str]:
        return await gt.run(hub, v["asker"], command.format(**v))

    await run("spawnclient fakey")
    # Unlike the xClient itself, a fake is not joined and parted for the occasion
    assert await run("as fakey op {c} bob") == []
    assert (await chaninfo(hub, v["asker"], CHAN)).members["bob"] == "none"


@pytest.mark.asyncio
async def test_the_server_needs_no_membership_and_no_ops(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    v = await _setup(hub)

    async def run(command: str) -> list[str]:
        return await gt.run(hub, v["asker"], command.format(**v))

    assert await run("servop {c} bob") == [f"{v['srv']} M {CHAN} +o {v['bob']} {v['ts']}"]
    assert await run("servmode {c} +R") == [f"{v['srv']} M {CHAN} +R {v['ts']}"]
    assert await run("servclearmode {c} o") == [f"{v['srv']} CM {CHAN} :o"]

    info = await chaninfo(hub, v["asker"], CHAN)
    assert info.members == {"alice": "none", "bob": "none", "carol": "none"}
    assert info.modes == "Rnt"


@pytest.mark.asyncio
async def test_kick_as_a_fake_client_and_as_the_server(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    v = await _setup(hub)

    async def run(command: str) -> list[str]:
        return await gt.run(hub, v["asker"], command.format(**v))

    v["fake"] = _introduced(await run("spawnclient fakey"), "N")
    await run("spawnjoin fakey {c}")

    # A fake client without ops cannot kick
    assert await run("as fakey kick {c} bob out") == []
    await hub.send_raw(f"{hub.server_numnick} M {CHAN} +o {v['fake']} {v['ts']}")

    assert await run("as fakey kick {c} bob out you go") == [
        f"{v['fake']} K {CHAN} {v['bob']} :out you go"
    ]
    # The gnuworld server itself: MyUplink->Kick() with no last argument
    assert await run("servkick {c} carol and you") == [f"{v['srv']} K {CHAN} {v['carol']} :and you"]
    assert await run("servkick {c} alice last one") == [f"{v['srv']} K {CHAN} {v['alice']} :last one"]

    # The kicked are off the channel in gnuworld's own view as well
    assert (await chaninfo(hub, v["asker"], CHAN)).members == {"fakey": "+o"}


@pytest.mark.asyncio
async def test_kicking_the_last_member_removes_the_channel(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, PERMIT_ACCOUNT)
    loner = await hub.introduce_nick("loner", username="loner")
    ts = int(time.time()) - 3600
    await hub.send_raw(f"{hub.server_numnick} B #lonely {ts} +tn {loner}:o")

    assert (await chaninfo(hub, asker, "#lonely")).found
    sent = await gt.run(hub, asker, "servkick #lonely loner bye")
    assert [line.split(" ", 1)[1] for line in sent] == [f"K #lonely {loner} :bye"]
    assert not (await chaninfo(hub, asker, "#lonely")).found


@pytest.mark.asyncio
async def test_a_network_service_is_not_kicked(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    v = await _setup(hub)
    service = await hub.introduce_nick("service", username="service", modes="+ik")
    await hub.send_raw(f"{service} J {CHAN} {v['ts']}")

    assert await gt.run(hub, v["asker"], f"servkick {CHAN} service no") == []
    assert "service" in (await chaninfo(hub, v["asker"], CHAN)).members


@pytest.mark.asyncio
async def test_topic_as_a_fake_client_and_as_the_server(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    v = await _setup(hub)

    async def run(command: str) -> list[str]:
        return await gt.run(hub, v["asker"], command.format(**v))

    def topic_line(source: str, text: str, sent: list[str]) -> bool:
        # "<source> T <#chan> <chan-ts> <topic-ts> :<text>"
        assert len(sent) == 1, sent
        fields, trailing = sent[0].split(" :", 1)
        parts = fields.split(" ")
        return (parts[:4] == [source, "T", CHAN, str(v["ts"])]
                and abs(int(parts[4]) - time.time()) < 300 and trailing == text)

    v["fake"] = _introduced(await run("spawnclient fakey"), "N")
    await run("spawnjoin fakey {c}")

    # The channel is +t and the fake client is not opped
    assert await run("as fakey topic {c} not allowed") == []
    assert (await chaninfo(hub, v["asker"], CHAN)).topic == ""

    # A server is not asked
    assert topic_line(v["srv"], "from the server", await run("servtopic {c} from the server"))

    await hub.send_raw(f"{hub.server_numnick} M {CHAN} +o {v['fake']} {v['ts']}")
    assert topic_line(v["fake"], "from a fake client", await run("as fakey topic {c} from a fake client"))
    assert (await chaninfo(hub, v["asker"], CHAN)).topic == "from a fake client"


@pytest.mark.asyncio
async def test_only_a_client_can_invite(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    v = await _setup(hub)

    async def run(command: str) -> list[str]:
        return await gt.run(hub, v["asker"], command.format(**v))

    v["fake"] = _introduced(await run("spawnclient fakey"), "N")

    # From a server an INVITE is a protocol violation, so none is sent
    assert await run("servinvite {c}") == []

    # The invitee by numnick and the channel's creation time, as P11 has it
    assert await run("as fakey invite {c}") == [f"{v['fake']} I {v['asker']} {CHAN} {v['ts']}"]
