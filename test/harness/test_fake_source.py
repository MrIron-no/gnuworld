"""Channel changes made by a fake client, or by a server gnuworld spawned.

Those exist only as an iClient or an iServer, which has no methods to call. The
server-side API therefore takes them as a Source, its last argument:

    MyUplink->Op(theChan, target, fakeClient);

A fake client is held to what the network would hold it to: it has to be on the
channel, opped. A spawned server is a server, and needs neither.
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
    # A client may not touch a server-only mode
    assert await run("as fakey mode {c} +R") == []

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
async def test_a_spawned_server_changes_a_channel(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    v = await _setup(hub)

    async def run(command: str) -> list[str]:
        return await gt.run(hub, v["asker"], command.format(**v))

    v["fakesrv"] = _introduced(await run("spawnserver fake.testnet A spawned server"), "S")
    assert v["fakesrv"] != v["srv"]

    # A server needs no membership and no ops
    assert await run("as fake.testnet op {c} bob") == [
        f"{v['fakesrv']} M {CHAN} +o {v['bob']} {v['ts']}"
    ]
    assert await run("as fake.testnet mode {c} +R") == [f"{v['fakesrv']} M {CHAN} +R {v['ts']}"]
    assert await run("as fake.testnet clearmode {c} o") == [f"{v['fakesrv']} CM {CHAN} :o"]

    info = await chaninfo(hub, v["asker"], CHAN)
    assert info.members == {"alice": "none", "bob": "none", "carol": "none"}
    assert info.modes == "Rnt"


@pytest.mark.asyncio
async def test_kick_through_each_kind_of_source(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    v = await _setup(hub)

    async def run(command: str) -> list[str]:
        return await gt.run(hub, v["asker"], command.format(**v))

    v["fake"] = _introduced(await run("spawnclient fakey"), "N")
    v["fakesrv"] = _introduced(await run("spawnserver fake.testnet A spawned server"), "S")
    await run("spawnjoin fakey {c}")

    # A fake client without ops cannot kick
    assert await run("as fakey kick {c} bob out") == []
    await hub.send_raw(f"{hub.server_numnick} M {CHAN} +o {v['fake']} {v['ts']}")

    assert await run("as fakey kick {c} bob out you go") == [
        f"{v['fake']} K {CHAN} {v['bob']} :out you go"
    ]
    assert await run("as fake.testnet kick {c} carol and you") == [
        f"{v['fakesrv']} K {CHAN} {v['carol']} :and you"
    ]
    # The gnuworld server itself: MyUplink->Kick() with no Source
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
    await gt.run(hub, asker, "spawnserver fake.testnet A spawned server")

    assert (await chaninfo(hub, asker, "#lonely")).found
    sent = await gt.run(hub, asker, "as fake.testnet kick #lonely loner bye")
    assert [line.split(" ", 1)[1] for line in sent] == [f"K #lonely {loner} :bye"]
    assert not (await chaninfo(hub, asker, "#lonely")).found


@pytest.mark.asyncio
async def test_a_network_service_is_not_kicked(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    v = await _setup(hub)
    service = await hub.introduce_nick("service", username="service", modes="+ik")
    await hub.send_raw(f"{service} J {CHAN} {v['ts']}")
    await gt.run(hub, v["asker"], "spawnserver fake.testnet A spawned server")

    assert await gt.run(hub, v["asker"], f"as fake.testnet kick {CHAN} service no") == []
    assert await gt.run(hub, v["asker"], f"servkick {CHAN} service no") == []
    assert "service" in (await chaninfo(hub, v["asker"], CHAN)).members


@pytest.mark.asyncio
async def test_topic_through_each_kind_of_source(gnutest_linked_p11):
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
    v["fakesrv"] = _introduced(await run("spawnserver fake.testnet A spawned server"), "S")
    await run("spawnjoin fakey {c}")

    # The channel is +t and the fake client is not opped
    assert await run("as fakey topic {c} not allowed") == []
    assert (await chaninfo(hub, v["asker"], CHAN)).topic == ""

    # A server is not asked
    assert topic_line(v["srv"], "from the server", await run("servtopic {c} from the server"))
    assert topic_line(v["fakesrv"], "from a spawned one",
                      await run("as fake.testnet topic {c} from a spawned one"))

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
    await run("spawnserver fake.testnet A spawned server")

    # From a server an INVITE is a protocol violation, so none is sent
    assert await run("servinvite {c}") == []
    assert await run("as fake.testnet invite {c}") == []

    # The invitee by numnick and the channel's creation time, as P11 has it
    assert await run("as fakey invite {c}") == [f"{v['fake']} I {v['asker']} {CHAN} {v['ts']}"]
