"""The lines gnuworld sends when one of its clients joins a channel with modes.

xServer::JoinChannel validates the modes, sends them through the mode engine
and updates the channel from the same typed changes. Before that it copied the
module's mode string to the network as given, and had a letter switch of its
own for the channel state. Recorded then, and different now:

  - Joining a keyed channel with another key sent "+k new" (refused by ircu, a
    key being set), then "-k old", then "+k new" again. Now one line, "-k+k".
  - Joining with the key the channel already has sent a pointless "+k".
  - A BURST with no modes had a doubled space where the mode block goes.
  - A mode missing its argument was logged and then indexed anyway.
"""

from __future__ import annotations

import time

import pytest

import gnutest_client as gt
from debugquery import chaninfo
from p10 import p10_token, strip_msg_tags

PERMIT_ACCOUNT = "MrIron"


async def _setup(hub) -> tuple[str, str, int]:
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, PERMIT_ACCOUNT)
    alice = await hub.introduce_nick("alice", username="alice")
    ts = int(time.time()) - 3600
    yy = hub.server_numnick
    await hub.send_raw(f"{yy} B #plain {ts} +tn {alice}:o")
    await hub.send_raw(f"{yy} B #keyed {ts} +tnk oldkey {alice}:o")
    await hub.send_raw(f"{yy} B #samekey {ts} +tnk samekey {alice}:o")
    return asker, gt.numnick(hub), ts


async def _create(hub, asker: str, me: str, command: str, channel: str) -> tuple[list[str], int]:
    """Run a command that creates ``channel``; return (lines after the C, its timestamp)."""
    sent = await gt.run(hub, asker, command)
    create = sent[0].split(" ")
    assert create[:3] == [me, "C", channel], sent
    return sent[1:], int(create[3])


@pytest.mark.asyncio
async def test_burst_line_without_modes_has_no_gap(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    bursts = [strip_msg_tags(line) for line in hub.received if p10_token(line) == "B"]
    assert len(bursts) == 1
    fields = bursts[0].split(" ")
    # "<server> B <#chan> <ts> <member>": five fields and no empty one
    assert fields[:3] == [hub.peer_numeric, "B", "#gnutest-opers"]
    assert fields[4:] == [gt.numnick(hub)]
    assert fields[3].isdigit()


@pytest.mark.asyncio
async def test_creating_a_channel_with_modes(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    asker, me, _ts = await _setup(hub)

    rest, created = await _create(hub, asker, me, "joinmodes #new1 +tn", "#new1")
    assert rest == [f"{me} M #new1 +tn {created}"]

    rest, created = await _create(hub, asker, me, "joinmodes #new2 +tnk sekrit", "#new2")
    assert rest == [f"{me} M #new2 +tnk sekrit {created}"]

    rest, created = await _create(hub, asker, me, "joinmodes #new3 +l 5", "#new3")
    assert rest == [f"{me} M #new3 +l 5 {created}"]

    rest, _created = await _create(hub, asker, me, "joinmodes #new4", "#new4")
    assert rest == []

    info = await chaninfo(hub, asker, "#new2")
    assert (info.modes, info.key) == ("knt", "sekrit")
    assert info.members == {"gnutest": "+o"}  # whoever creates a channel is opped
    assert (await chaninfo(hub, asker, "#new3")).limit == 5


@pytest.mark.asyncio
async def test_joining_an_existing_channel_with_modes(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    asker, me, ts = await _setup(hub)

    assert await gt.run(hub, asker, "joinmodes #plain +m") == [
        f"{me} J #plain {ts}",
        f"{me} M #plain +m {ts}",
    ]
    info = await chaninfo(hub, asker, "#plain")
    assert info.modes == "mnt"
    assert info.members["gnutest"] == "none"


@pytest.mark.asyncio
async def test_joining_a_keyed_channel_with_another_key(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    asker, me, ts = await _setup(hub)

    assert await gt.run(hub, asker, "joinops #keyed +k newkey") == [
        f"{me} J #keyed {ts}",
        f"{hub.peer_numeric} M #keyed +o {me} {ts}",
        f"{me} M #keyed -k+k oldkey newkey {ts}",
    ]
    info = await chaninfo(hub, asker, "#keyed")
    assert (info.key, info.members["gnutest"]) == ("newkey", "+o")

    # The key it already has: nothing to change, so nothing is sent for it
    assert await gt.run(hub, asker, "joinmodes #samekey +k samekey") == [f"{me} J #samekey {ts}"]
    assert (await chaninfo(hub, asker, "#samekey")).key == "samekey"


@pytest.mark.asyncio
async def test_invalid_join_modes_are_dropped_and_the_rest_sent(gnutest_linked_p11):
    hub, proc = gnutest_linked_p11
    asker, me, _ts = await _setup(hub)

    # 'x' does not exist and 'd' is local to an ircu server
    rest, created = await _create(hub, asker, me, "joinmodes #bad1 +txnd", "#bad1")
    assert rest == [f"{me} M #bad1 +tn {created}"]

    # "+l" and "+k" with nothing behind them used to be indexed regardless
    rest, created = await _create(hub, asker, me, "joinmodes #bad2 +tlk", "#bad2")
    assert rest == [f"{me} M #bad2 +t {created}"]

    # Members and bans are not modes to join with
    rest, created = await _create(hub, asker, me, "joinmodes #bad3 +nob alice *!*@x", "#bad3")
    assert rest == [f"{me} M #bad3 +n {created}"]

    assert (await chaninfo(hub, asker, "#bad2")).modes == "t"
    assert proc.proc is not None and proc.proc.returncode is None
