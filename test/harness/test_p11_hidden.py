"""P11 delayed-join (hidden) members: BURST ":d", joins to +D, and the reveals.

A member is hidden when it is burst in the ":d" group or joins a +D channel
without status. It is revealed by gaining op or voice, by setting the topic, or
by a P11 REVEAL (RV), which is what the network sends when it speaks. See ircu
doc/P11.md 8.1, 8.3 and 8.12. State is read back through mod.debug CHANINFO.
"""

from __future__ import annotations

import time

import pytest

from debugquery import chaninfo
from p10 import p10_token

PERMIT_ACCOUNT = "MrIron"
CHAN = "#hidden"


async def _chaninfo(hub, asker: str, channel: str) -> dict[str, str]:
    """{nick: "hide" | "none" | "+v" | "+o" | "+o+v"} as mod.debug sees the channel."""
    return (await chaninfo(hub, asker, channel)).members


async def _setup(hub) -> tuple[str, dict[str, str], int]:
    """Introduce the cast and burst CHAN with a ":d" group. Returns (asker, numnicks, ts)."""
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, PERMIT_ACCOUNT)

    n = {nick: await hub.introduce_nick(nick, username=nick) for nick in
         ("plain", "hid1", "hid2", "hid3", "hid4", "voiced", "opped")}

    ts = int(time.time()) - 3600
    # Mandatory group order: no status, hidden, voice, op. ":d" carries to the
    # suffix-less numerics behind it, exactly like ":o" and ":v" do.
    members = (f"{n['plain']},{n['hid1']}:d,{n['hid2']},{n['hid3']},{n['hid4']},"
               f"{n['voiced']}:v,{n['opped']}:o")
    await hub.send_raw(f"{hub.server_numnick} B {CHAN} {ts} +tn {members}")
    return asker, n, ts


@pytest.mark.asyncio
async def test_burst_d_group_is_hidden(debug_linked_p11):
    hub, _proc = debug_linked_p11
    asker, _n, _ts = await _setup(hub)

    assert await _chaninfo(hub, asker, CHAN) == {
        "plain": "none",
        "hid1": "hide", "hid2": "hide", "hid3": "hide", "hid4": "hide",
        "voiced": "+v",
        "opped": "+o",
    }


@pytest.mark.asyncio
async def test_reveal_by_rv_voice_op_and_topic(debug_linked_p11):
    hub, _proc = debug_linked_p11
    asker, n, ts = await _setup(hub)

    await hub.send_raw(f"{n['hid1']} RV {CHAN} {ts}")                      # spoke
    await hub.send_raw(f"{n['opped']} M {CHAN} +v {n['hid2']} {ts}")       # voiced
    await hub.send_raw(f"{n['opped']} M {CHAN} +o {n['hid3']} {ts}")       # opped
    await hub.send_raw(f"{n['hid4']} T {CHAN} {ts} {ts} :set by a hidden member")

    assert await _chaninfo(hub, asker, CHAN) == {
        "plain": "none",
        "hid1": "none",
        "hid2": "+v",
        "hid3": "+o",
        "hid4": "none",
        "voiced": "+v",
        "opped": "+o",
    }


@pytest.mark.asyncio
async def test_rv_for_a_newer_channel_is_ignored(debug_linked_p11):
    hub, _proc = debug_linked_p11
    asker, n, ts = await _setup(hub)

    # The reveal is about a younger incarnation of the channel: it lost the
    # timestamp race and must not touch the channel we know.
    await hub.send_raw(f"{n['hid1']} RV {CHAN} {ts + 100}")
    assert (await _chaninfo(hub, asker, CHAN))["hid1"] == "hide"

    # An older or equal timestamp is accepted.
    await hub.send_raw(f"{n['hid1']} RV {CHAN} {ts - 100}")
    assert (await _chaninfo(hub, asker, CHAN))["hid1"] == "none"


@pytest.mark.asyncio
async def test_things_that_do_not_reveal(debug_linked_p11):
    hub, _proc = debug_linked_p11
    asker, n, ts = await _setup(hub)

    # Speaking is announced by RV, never inferred from the message itself,
    # and a nick change is simply not shown to the channel.
    await hub.send_raw(f"{n['hid1']} P {CHAN} :not a reveal without RV")
    await hub.send_raw(f"{n['hid2']} N hid2renamed {int(time.time())}")

    info = await _chaninfo(hub, asker, CHAN)
    assert info["hid1"] == "hide"
    assert info["hid2renamed"] == "hide"


@pytest.mark.asyncio
async def test_join_is_hidden_only_on_a_plus_d_channel(debug_linked_p11):
    hub, _proc = debug_linked_p11
    asker, n, ts = await _setup(hub)
    late = await hub.introduce_nick("late", username="late")

    await hub.send_raw(f"{late} J {CHAN} {ts}")
    assert (await _chaninfo(hub, asker, CHAN))["late"] == "none"

    await hub.send_raw(f"{late} L {CHAN}")
    await hub.send_raw(f"{n['opped']} M {CHAN} +D {ts}")
    await hub.send_raw(f"{late} J {CHAN} {ts}")
    assert (await _chaninfo(hub, asker, CHAN))["late"] == "hide"

    # -D does not reveal anyone: the member stays hidden until it acts.
    await hub.send_raw(f"{n['opped']} M {CHAN} -D {ts}")
    assert (await _chaninfo(hub, asker, CHAN))["late"] == "hide"


async def _userinfo_channels(hub, asker: str, nick: str) -> str:
    """PRIVMSG debug USERINFO and return the text of its "On channels:" notice."""
    after = len(hub.received)
    await hub.send_privmsg(asker, f"debug@{hub.peer_name}", f"USERINFO {nick}")

    def is_channels(line: str) -> bool:
        return p10_token(line) == "O" and "On channels:" in line

    line = await hub.wait_for(is_channels, timeout=10.0, after=after)
    return line.split("On channels:", 1)[1].strip()


@pytest.mark.asyncio
async def test_userinfo_marks_a_hidden_membership(debug_linked_p11):
    """USERINFO prefixes the channel with '<', as ircu does in WHOIS."""
    hub, _proc = debug_linked_p11
    asker, n, ts = await _setup(hub)

    assert await _userinfo_channels(hub, asker, "hid1") == f"<{CHAN}"
    assert await _userinfo_channels(hub, asker, "plain") == CHAN
    assert await _userinfo_channels(hub, asker, "opped") == f"@{CHAN}"

    await hub.send_raw(f"{n['hid1']} RV {CHAN} {ts}")
    assert await _userinfo_channels(hub, asker, "hid1") == CHAN


@pytest.mark.asyncio
async def test_p10_uplink_never_flags_anyone_hidden(debug_linked):
    """P10 has no REVEAL, so the state could not be kept correct: not tracked.

    A P10 hub bursts a hidden member as a plain one, and ircu infers "hidden"
    from +D. gnuworld deliberately does not: it would only learn of a reveal
    by speech on the few channels it has a client on.
    """
    hub, _proc = debug_linked
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, PERMIT_ACCOUNT)
    plain = await hub.introduce_nick("plain", username="plain")
    opped = await hub.introduce_nick("opped", username="opped")
    late = await hub.introduce_nick("late", username="late")

    ts = int(time.time()) - 3600
    await hub.send_raw(f"{hub.server_numnick} B {CHAN} {ts} +tnD {plain},{opped}:o")
    await hub.send_raw(f"{late} J {CHAN} {ts}")

    assert await _chaninfo(hub, asker, CHAN) == {"plain": "none", "opped": "+o", "late": "none"}
