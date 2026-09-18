"""KICK and KILL, by the rules of ircu (channel.c make_zombie(), m_kick.c,
m_kill.c; doc/P11.md 8.5).

KICK. ircu keeps a kicked member on the channel as a zombie until the member's
own server confirms the kick with a PART, so that what the victim sent before
it knew is still matched and not bounced. gnuworld bounces nothing, and ircu
treats a zombie as no member, so gnuworld takes the member off at once: the
PART that follows finds nobody, and a kick that the network bounces brings the
victim back with a JOIN. The exception is a client of ours: the server that
has to confirm is gnuworld, for a client on a server a module spawned too.

KILL needs no confirmation. Every server removes the victim as the KILL
passes; the victim's own server sends a KILL back the way it came, for a
numeric that was given out again meanwhile.
"""

from __future__ import annotations

import time

import pytest

import gnutest_client as gt
from debugquery import chaninfo
from p10 import p10_token

PERMIT_ACCOUNT = "MrIron"
CHAN = "#kk"


async def _setup(hub):
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, PERMIT_ACCOUNT)
    n = {nick: await hub.introduce_nick(nick, username=nick) for nick in ("alice", "bob")}
    ts = int(time.time()) - 3600
    await hub.send_raw(f"{hub.server_numnick} B {CHAN} {ts} +tn {n['bob']},{n['alice']}:o")
    return asker, n, ts


def _numnick(lines: list[str], nick: str) -> str:
    return next(l for l in lines if f" N {nick} " in l).split(" :", 1)[0].split(" ")[-1]


async def _members(hub, asker: str) -> set[str]:
    """Who is on the channel, in gnuworld's own view (mod.debug is loaded too)."""
    info = await chaninfo(hub, asker, CHAN)
    return set(info.members) if info.found else set()


@pytest.mark.asyncio
async def test_kick_of_a_client_on_a_spawned_server_is_confirmed_by_us(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    asker, n, ts = await _setup(hub)

    await gt.run(hub, asker, "spawnserver spawned.testnet A spawned server")
    far = _numnick(await gt.run(hub, asker, "spawnclient farclone spawned.testnet"), "farclone")
    assert far[:2] != hub.peer_numeric, "the clone should sit on the spawned server"
    await gt.run(hub, asker, f"spawnjoin farclone {CHAN}")

    after = len(hub.received)
    await hub.send_raw(f"{n['alice']} K {CHAN} {far} :out")
    confirmed = await hub.wait_for(lambda l: p10_token(l) == "L" and far in l, timeout=10.0, after=after)
    assert confirmed.endswith(f"{far} L {CHAN}")


@pytest.mark.asyncio
async def test_we_confirm_our_own_kick_of_a_client_of_ours(gnutest_linked_p11):
    """Kicker on this server, victim on a spawned one: the uplink makes the
    victim a zombie and waits for its server, which is us."""
    hub, _proc = gnutest_linked_p11
    asker, _n, _ts = await _setup(hub)

    await gt.run(hub, asker, "spawnserver spawned.testnet A spawned server")
    far = _numnick(await gt.run(hub, asker, "spawnclient farclone spawned.testnet"), "farclone")
    await gt.run(hub, asker, f"spawnjoin farclone {CHAN}")

    sent = [l for l in await gt.run(hub, asker, f"servkick {CHAN} farclone bye") if p10_token(l) in ("K", "L")]
    assert sent == [f"{hub.peer_numeric} K {CHAN} {far} :bye", f"{far} L {CHAN}"]

    # Somebody else's client is confirmed by its own server, not by us
    sent = [l for l in await gt.run(hub, asker, f"servkick {CHAN} bob bye") if p10_token(l) in ("K", "L")]
    assert [p10_token(l) for l in sent] == ["K"]


@pytest.mark.asyncio
async def test_the_part_that_confirms_a_kick_finds_nobody_and_a_bounce_brings_the_victim_back(
    gnutest_linked_p11,
):
    hub, proc = gnutest_linked_p11
    asker, n, ts = await _setup(hub)

    # Inbound: gone at once, and the PART of the victim's server is no news
    await hub.send_raw(f"{n['alice']} K {CHAN} {n['bob']} :out")
    await hub.send_raw(f"{n['bob']} L {CHAN}")
    assert "bob" not in await _members(hub, asker)

    # Our kick of alice, bounced by the network: she is back, with her op
    await gt.run(hub, asker, f"servkick {CHAN} alice out")
    assert "alice" not in await _members(hub, asker)
    await hub.send_raw(f"{n['alice']} J {CHAN} {ts}")
    await hub.send_raw(f"{hub.server_numnick} M {CHAN} +o {n['alice']} {ts}")
    assert "alice" in await _members(hub, asker)
    assert proc.proc is not None and proc.proc.returncode is None


@pytest.mark.asyncio
async def test_a_join_to_a_channel_we_removed_keeps_the_timestamp_it_carries(debug_linked_p11):
    hub, _proc = debug_linked_p11
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, PERMIT_ACCOUNT)
    alice = await hub.introduce_nick("alice", username="alice")
    ts = int(time.time()) - 3600

    await hub.send_raw(f"{alice} J #gone {ts}")
    assert (await chaninfo(hub, asker, "#gone")).created == ts


@pytest.mark.asyncio
async def test_kill_on_a_p11_link(gnutest_linked_p11):
    await _kill(*gnutest_linked_p11, p11=True)


@pytest.mark.asyncio
async def test_kill_on_a_p10_link(gnutest_linked_p10):
    await _kill(*gnutest_linked_p10, p11=False)


async def _kill(hub, proc, p11: bool) -> None:
    asker, n, _ts = await _setup(hub)
    hub_yy, srv = hub.server_numnick, hub.peer_numeric

    def kill(victim: str, path: str, reason: str) -> str:
        return f"{hub_yy} D {victim} {path} :{reason}" if p11 else f"{hub_yy} D {victim} :{path} {reason}"

    # Somebody else's client: gone, and nothing is sent back
    after = len(hub.received)
    await hub.send_raw(kill(n["bob"], "hub.testnet!oper", "(bye)"))
    assert "bob" not in await _members(hub, asker)
    assert not [l for l in hub.received[after:] if p10_token(l) == "D"]

    # A client of ours: the KILL goes back the way it came, as ircu has it
    fakey = _numnick(await gt.run(hub, asker, "spawnclient fakey"), "fakey")
    after = len(hub.received)
    await hub.send_raw(kill(fakey, "hub.testnet!oper", "(bye)"))
    back = await hub.wait_for(lambda l: p10_token(l) == "D", timeout=10.0, after=after)
    assert back.endswith(
        f"{srv} D {fakey} hub.testnet!oper :(Ghost 5 Numeric Collided)" if p11
        else f"{srv} D {fakey} :hub.testnet!oper (Ghost 5 Numeric Collided)"
    )

    # Our own kill: the victim is gone at once, and the KILL its server sends
    # back finds nobody here either
    sent = [l for l in await gt.run(hub, asker, "servkill alice enough") if p10_token(l) == "D"]
    assert len(sent) == 1 and sent[0].startswith(f"{srv} D {n['alice']} ")
    await hub.send_raw(kill(n["alice"], "leaf.testnet", "(Ghost 5 Numeric Collided)"))
    assert "alice" not in await _members(hub, asker)
    assert proc.proc is not None and proc.proc.returncode is None
