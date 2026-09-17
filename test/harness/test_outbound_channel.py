"""Characterisation: the exact lines gnuworld sends for each channel operation.

Every step makes mod.gnutest call one core API function and compares what
gnuworld put on the wire, byte for byte. This is the "before" picture for the
mode engine: once mode lines are produced in one place, each difference from
what is recorded here must be a deliberate one.

On a P11 link a channel MODE without an all-digit final timestamp is dropped by
the receiver (ircu doc/P11.md 8.7), so the timestamp is asserted everywhere.

KNOWN DEFECTS recorded below as they are today, for the mode engine to fix:
  [spaces]    a doubled or tripled space before the timestamp. Harmless on the
              wire, but it shows each call site formats its own line.
  [clearmode] clearing bans as a non-oper sends "-b <ts>" with no mask, which
              cannot remove anything: the receiver reads the timestamp as the mask.
  [split]     a combined "+ov"/"-lk" is sent as "+o+v"/"-l-k". Valid, just noisy.
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
    ("op {c} bob carol", ["{me} M {c} +oo {bob} {carol}  {ts}"]),  # [spaces]
    ("deop {c} bob carol", ["{me} M {c} -oo {bob} {carol} {ts}"]),
    ("voice {c} bob", ["{me} M {c} +v {bob} {ts}"]),
    ("devoice {c} bob", ["{me} M {c} -v {bob} {ts}"]),
    ("voice {c} bob carol", ["{me} M {c} +vv {bob} {carol}  {ts}"]),  # [spaces]
    ("devoice {c} bob carol", ["{me} M {c} -vv {bob} {carol} {ts}"]),

    ("ban {c} bob", ["{me} M {c} +b {bobmask} {ts}"]),
    ("unban {c} {bobmask}", ["{me} M {c} -b {bobmask} {ts}"]),
    ("ban {c} bob carol", ["{me} M {c} +bb {bobmask} {carolmask}  {ts}"]),  # [spaces]
    ("unban {c} {bobmask} {carolmask}", ["{me} M {c} -bb {bobmask} {carolmask}  {ts}"]),  # [spaces]
    ("banmask {c} *!*@one.example *!*@two.example",
     ["{me} M {c} +bb *!*@one.example *!*@two.example  {ts}"]),  # [spaces]
    ("unban {c} *!*@one.example *!*@two.example",
     ["{me} M {c} -bb *!*@one.example *!*@two.example  {ts}"]),  # [spaces]

    ("mode {c} +m", ["{me} M {c} +m   {ts}"]),  # [spaces]
    ("mode {c} -m", ["{me} M {c} -m   {ts}"]),  # [spaces]
    ("mode {c} +l 10", ["{me} M {c} +l 10  {ts}"]),  # [spaces]
    ("mode {c} +k sekrit", ["{me} M {c} +k sekrit  {ts}"]),  # [spaces]
    ("mode {c} -lk sekrit", ["{me} M {c} -l-k  sekrit  {ts}"]),  # [spaces] [split]
    ("mode {c} +ov bob carol", ["{me} M {c} +o+v {bob} {carol}  {ts}"]),  # [spaces] [split]
    ("mode {c} -ov bob carol", ["{me} M {c} -o-v {bob} {carol}  {ts}"]),  # [spaces] [split]
    ("mode {c} +b *!*@three.example", ["{me} M {c} +b *!*@three.example  {ts}"]),  # [spaces]
    ("servmode {c} +i", ["{srv} M {c} +i   {ts}"]),  # [spaces]
    ("servmode {c} -i", ["{srv} M {c} -i   {ts}"]),  # [spaces]
    ("servop {c} bob", ["{srv} M {c} +o {bob}  {ts}"]),  # [spaces]
    ("deop {c} bob", ["{me} M {c} -o {bob} {ts}"]),

    ("topic {c} hello there", ["{me} T {c} :hello there"]),
    ("invite {c}", ["{me} I {asker} {c}"]),
    ("kick {c} dave go away", ["{me} K {c} {dave} :go away"]),
    ("servkick {c} carol and you", ["{srv} K {c} {carol} :and you"]),
    ("bankick {c} bob bye now", [
        "{me} M {c} +b {bobmask} {ts}",
        "{me} K {c} {bob} :bye now",
    ]),
    ("clearmode {c} b", ["{me} M {c} -b {ts}"]),  # [clearmode]
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
