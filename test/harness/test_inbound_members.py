"""How a member gets its op: CREATE, a JOIN that creates, CLEARMODE.

ChannelUser's setters are not public. A member that arrives with a mode is
constructed with it, and every later change goes through
xServer::OnChannelModeO() and OnChannelModeV().
"""

from __future__ import annotations

import time

import pytest

from debugquery import chaninfo
from test_inbound_modes import _setup


@pytest.mark.asyncio
async def test_create_ops_the_creator(debug_linked_p11):
    hub, _proc = debug_linked_p11
    asker, n, _ts = await _setup(hub)
    now = int(time.time())

    await hub.send_raw(f"{n['plain']} C #made {now}")
    info = await chaninfo(hub, asker, "#made")
    assert info.created == now
    assert info.members == {"plain": "+o"}


@pytest.mark.asyncio
async def test_create_with_an_older_timestamp_takes_the_channel_over(debug_linked_p11):
    hub, _proc = debug_linked_p11
    asker, n, _ts = await _setup(hub)
    now = int(time.time())

    await hub.send_raw(f"{n['plain']} C #made {now}")
    await hub.send_raw(f"{n['plain']} M #made +i {now}")
    await hub.send_raw(f"{n['other']} C #made {now - 100}")
    info = await chaninfo(hub, asker, "#made")
    assert info.created == now - 100
    assert info.modes == ""
    assert info.members["other"] == "+o"

    # A younger CREATE lost the race: on the channel, but not an op
    await hub.send_raw(f"{n['opped']} C #made {now + 100}")
    info = await chaninfo(hub, asker, "#made")
    assert info.created == now - 100
    assert info.members["opped"] == "none"


@pytest.mark.asyncio
async def test_join_to_a_channel_we_do_not_know_is_a_create(debug_linked_p11):
    hub, _proc = debug_linked_p11
    asker, n, _ts = await _setup(hub)
    now = int(time.time())

    await hub.send_raw(f"{n['plain']} J #joined {now}")
    await hub.send_raw(f"{n['other']} J #joined {now}")
    info = await chaninfo(hub, asker, "#joined")
    assert info.members == {"plain": "+o", "other": "none"}


@pytest.mark.asyncio
async def test_one_join_to_several_channels(debug_linked_p11):
    """A JOIN may name several channels: a member of each, and the op only
    where the JOIN created the channel."""
    hub, _proc = debug_linked_p11
    asker, n, _ts = await _setup(hub)
    now = int(time.time())

    await hub.send_raw(f"{n['other']} C #first {now}")
    await hub.send_raw(f"{n['plain']} J #first,#second,#third {now}")

    assert (await chaninfo(hub, asker, "#first")).members == {"other": "+o", "plain": "none"}
    assert (await chaninfo(hub, asker, "#second")).members == {"plain": "+o"}
    assert (await chaninfo(hub, asker, "#third")).members == {"plain": "+o"}

    # Each membership is its own: a change in one channel leaves the others
    await hub.send_raw(f"{hub.server_numnick} M #second -o {n['plain']} {now}")
    assert (await chaninfo(hub, asker, "#second")).members == {"plain": "none"}
    assert (await chaninfo(hub, asker, "#third")).members == {"plain": "+o"}


@pytest.mark.asyncio
async def test_a_kick_of_a_client_of_ours_is_confirmed_with_a_part(gnutest_linked_p11):
    """The network keeps a kicked member as a zombie until its own server
    confirms with a PART. For a fake client of ours that server is gnuworld;
    mod.cloner and mod.dronescan each wrote that PART themselves."""
    import gnutest_client as gt

    hub, _proc = gnutest_linked_p11
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, "MrIron")
    alice = await hub.introduce_nick("alice", username="alice")
    ts = int(time.time()) - 3600
    await hub.send_raw(f"{hub.server_numnick} B #kicks {ts} +tn {alice}:o")

    introduced = await gt.run(hub, asker, "spawnclient fakey")
    fakey = next(line for line in introduced if " N fakey " in line).split(" :", 1)[0].split(" ")[-1]
    await gt.run(hub, asker, "spawnjoin fakey #kicks")

    after = len(hub.received)
    await hub.send_raw(f"{alice} K #kicks {fakey} :out")
    confirmed = await hub.wait_for(lambda line: f"{fakey} L #kicks" in line, timeout=10.0, after=after)
    assert confirmed.endswith(f"{fakey} L #kicks")

    # A kick of somebody else's client is theirs to confirm
    bob = await hub.introduce_nick("bob", username="bob")
    await hub.send_raw(f"{bob} J #kicks {ts}")
    after = len(hub.received)
    await hub.send_raw(f"{alice} K #kicks {bob} :out")
    await gt.run(hub, asker, "help")
    assert not [line for line in hub.received[after:] if " L #kicks" in line]
