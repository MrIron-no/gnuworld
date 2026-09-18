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
