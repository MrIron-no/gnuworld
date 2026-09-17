"""Characterisation: the exact lines gnuworld sends for each channel operation.

Every step makes mod.gnutest call one core API function and compares what
gnuworld put on the wire, byte for byte. This is the "before" picture for the
mode engine: once mode lines are produced in one place, each difference from
what is recorded here must be a deliberate one.

On a P11 link a channel MODE without an all-digit final timestamp is dropped by
the receiver (ircu doc/P11.md 8.7), so the timestamp is asserted everywhere.

The mode engine (libgnuworld/ChannelModes, xServer::SendChannelModes) now
formats every one of these lines. Compared with what was first recorded here:
exactly one space between fields, where call sites used to leave two or three
before the timestamp; "+ov" and "-lk" instead of "+o+v" and "-l-k"; and a
non-oper ClearMode of bans names each mask, where it used to send "-b <ts>",
which removed nothing.
"""

from __future__ import annotations

import time

import pytest

import gnutest_client as gt
from debugquery import chaninfo

CHAN = "#modes"
AWAY = "#away"
PERMIT_ACCOUNT = "MrIron"

# (command, [lines gnuworld is expected to send]); formatted with the numerics below.
STEPS: list[tuple[str, list[str]]] = [
    # Not on the channel: join, take ops from the server, act, leave again
    ("op {away} bob", [
        "{me} J {away} {ts}",
        "{srv} M {away} +o {me} {ts}",
        "{me} M {away} +o {bob} {ts}",
        "{me} L {away} :",
    ]),
    ("join {c}", ["{me} J {c} {ts}"]),
    ("@op-gnutest", []),  # the hub ops gnutest, so the rest is sent with ops held

    ("op {c} bob", ["{me} M {c} +o {bob} {ts}"]),
    ("deop {c} bob", ["{me} M {c} -o {bob} {ts}"]),
    ("op {c} bob carol", ["{me} M {c} +oo {bob} {carol} {ts}"]),
    ("deop {c} bob carol", ["{me} M {c} -oo {bob} {carol} {ts}"]),
    ("voice {c} bob", ["{me} M {c} +v {bob} {ts}"]),
    ("devoice {c} bob", ["{me} M {c} -v {bob} {ts}"]),
    ("voice {c} bob carol", ["{me} M {c} +vv {bob} {carol} {ts}"]),
    ("devoice {c} bob carol", ["{me} M {c} -vv {bob} {carol} {ts}"]),

    ("ban {c} bob", ["{me} M {c} +b {bobmask} {ts}"]),
    ("unban {c} {bobmask}", ["{me} M {c} -b {bobmask} {ts}"]),
    ("ban {c} bob carol", ["{me} M {c} +bb {bobmask} {carolmask} {ts}"]),
    ("unban {c} {bobmask} {carolmask}", ["{me} M {c} -bb {bobmask} {carolmask} {ts}"]),
    ("banmask {c} *!*@one.example *!*@two.example",
     ["{me} M {c} +bb *!*@one.example *!*@two.example {ts}"]),
    ("unban {c} *!*@one.example *!*@two.example",
     ["{me} M {c} -bb *!*@one.example *!*@two.example {ts}"]),

    ("mode {c} +m", ["{me} M {c} +m {ts}"]),
    ("mode {c} -m", ["{me} M {c} -m {ts}"]),
    ("mode {c} +l 10", ["{me} M {c} +l 10 {ts}"]),
    ("mode {c} +k sekrit", ["{me} M {c} +k sekrit {ts}"]),
    ("mode {c} -lk sekrit", ["{me} M {c} -lk sekrit {ts}"]),
    ("mode {c} +ov bob carol", ["{me} M {c} +ov {bob} {carol} {ts}"]),
    ("mode {c} -ov bob carol", ["{me} M {c} -ov {bob} {carol} {ts}"]),
    ("mode {c} +b *!*@three.example", ["{me} M {c} +b *!*@three.example {ts}"]),
    ("servmode {c} +i", ["{srv} M {c} +i {ts}"]),
    ("servmode {c} -i", ["{srv} M {c} -i {ts}"]),
    ("servop {c} bob", ["{srv} M {c} +o {bob} {ts}"]),
    ("deop {c} bob", ["{me} M {c} -o {bob} {ts}"]),

    ("topic {c} hello there", ["{me} T {c} :hello there"]),
    ("invite {c}", ["{me} I {asker} {c}"]),
    ("kick {c} dave go away", ["{me} K {c} {dave} :go away"]),
    # xClient::Kick(..., true): the bool form, still there for the modules
    ("kickasserver {c} carol and you", ["{srv} K {c} {carol} :and you"]),
    ("bankick {c} bob bye now", [
        "{me} M {c} +b {bobmask} {ts}",
        "{me} K {c} {bob} :bye now",
    ]),
    # three.example was set above and never removed; bob's mask came with the bankick
    ("clearmode {c} b", ["{me} M {c} -bb {bobmask} *!*@three.example {ts}"]),
    ("part {c}", ["{me} L {c} :"]),
]


@pytest.mark.asyncio
async def test_lines_sent_for_each_channel_operation(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11

    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, PERMIT_ACCOUNT)
    n = {
        nick: await hub.introduce_nick(nick, username=nick, host=f"{nick}.example.net")
        for nick in ("alice", "bob", "carol", "dave")
    }
    ts = int(time.time()) - 3600
    hub_yy = hub.server_numnick
    await hub.send_raw(
        f"{hub_yy} B {CHAN} {ts} +tn {n['bob']},{n['carol']},{n['dave']},{n['alice']}:o"
    )
    await hub.send_raw(f"{hub_yy} B {AWAY} {ts} +tn {n['bob']},{n['alice']}:o")

    names = dict(
        n,
        c=CHAN,
        away=AWAY,
        ts=ts,
        asker=asker,
        me=gt.numnick(hub),
        srv=hub.peer_numeric,
        bobmask="*!bob@bob.example.net",
        carolmask="*!carol@carol.example.net",
    )

    for command, expected in STEPS:
        if command == "@op-gnutest":
            await hub.send_raw(f"{hub_yy} M {CHAN} +o {names['me']} {ts}")
            continue
        sent = await gt.run(hub, asker, command.format(**names))
        assert sent == [line.format(**names) for line in expected], f"after: {command}"


@pytest.mark.asyncio
async def test_api_calls_also_update_gnuworlds_own_state(gnutest_linked_p11):
    """An API call must change gnuworld's view of the channel, not only the wire."""
    hub, _proc = gnutest_linked_p11

    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, PERMIT_ACCOUNT)
    n = {
        nick: await hub.introduce_nick(nick, username=nick, host=f"{nick}.example.net")
        for nick in ("alice", "bob", "carol")
    }
    ts = int(time.time()) - 3600
    hub_yy = hub.server_numnick
    await hub.send_raw(f"{hub_yy} B {CHAN} {ts} +tn {n['bob']},{n['carol']},{n['alice']}:o")

    await gt.run(hub, asker, f"join {CHAN}")
    await hub.send_raw(f"{hub_yy} M {CHAN} +o {gt.numnick(hub)} {ts}")
    for command in (
        f"op {CHAN} bob",
        f"voice {CHAN} bob carol",
        f"banmask {CHAN} *!*@one.example",
        f"mode {CHAN} +ml 10",
        f"mode {CHAN} +k sekrit",
        f"topic {CHAN} hello there",
    ):
        await gt.run(hub, asker, command)

    info = await chaninfo(hub, asker, CHAN)
    assert info.members == {"alice": "+o", "bob": "+o+v", "carol": "+v", "gnutest": "+o"}
    assert info.bans == {"*!*@one.example"}
    assert info.modes == "klmnt"
    assert (info.key, info.limit) == ("sekrit", 10)
    assert info.topic == "hello there"
