"""xServer::BurstChannel(): claiming a channel with an older timestamp.

A module bursts a channel that already exists, with a timestamp older than the
network's, so that the network drops the channel's modes and bans and takes
ours; openchanfix does it for the channels it fixes. It only works during
gnuworld's own burst, which follows the uplink's, so the channel is part of the
burst the fake hub sends and gnutest is configured to claim it.

BurstChannel() used to copy the module's mode string into the BURST as given
and had a letter switch of its own for the channel state. It validates first
now, builds the mode block in burst order, and sends nothing if the modes are
not valid.
"""

from __future__ import annotations

import time

import pytest

from conftest import link_debug
from debugquery import chaninfo
from p10 import p10_token, strip_msg_tags

PERMIT_ACCOUNT = "MrIron"
CHAN = "#claimed"


def _network_burst(hub, ts: int) -> list[str]:
    """One user on CHAN, which is +mik with a ban: all of it has to go.
    On a P11 link a ban is burst as "mask timestamp setter"."""
    yy = hub.server_numnick
    return [
        f"{yy} N alice 1 {ts} alice alice.example.net +i AAAAAA {yy}AAA :Alice",
        f"{yy} B {CHAN} {ts} +mik oldkey {yy}AAA:o :%*!*@banned.example.net {ts} alice",
    ]


def _bursts_for(hub, channel: str) -> list[str]:
    return [strip_msg_tags(l) for l in hub.received
            if p10_token(l) == "B" and strip_msg_tags(l).split(" ")[2] == channel]


async def _asker(hub) -> str:
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, PERMIT_ACCOUNT)
    return asker


@pytest.mark.asyncio
async def test_claiming_a_channel_with_an_older_timestamp(docker_stack, fake_hub_p11, tmp_path):
    hub = fake_hub_p11
    ts = int(time.time()) - 3600
    ours = ts - 1000

    async with link_debug(hub, tmp_path, burst=_network_burst(hub, ts), gnutest=True,
                          burstchannel=f"{CHAN} {ours} +ktnl sekrit 25") as (hub, _proc):
        # The mode block in burst order, t n before l before k, whatever order it was given in
        assert _bursts_for(hub, CHAN) == [f"{hub.peer_numeric} B {CHAN} {ours} +tnlk 25 sekrit"]

        info = await chaninfo(hub, await _asker(hub), CHAN)
        assert info.created == ours
        assert info.modes == "klnt"  # +m and +i went with the network's version
        assert (info.key, info.limit) == ("sekrit", 25)
        assert info.bans == set()
        assert "alice" in info.members  # taking a channel over does not empty it


@pytest.mark.asyncio
async def test_claiming_without_modes_leaves_no_gap(docker_stack, fake_hub_p11, tmp_path):
    hub = fake_hub_p11
    ts = int(time.time()) - 3600

    async with link_debug(hub, tmp_path, burst=_network_burst(hub, ts), gnutest=True,
                          burstchannel=f"{CHAN} {ts - 1000}") as (hub, _proc):
        assert _bursts_for(hub, CHAN) == [f"{hub.peer_numeric} B {CHAN} {ts - 1000}"]
        assert (await chaninfo(hub, await _asker(hub), CHAN)).modes == ""


@pytest.mark.parametrize("modes", ["+tn-m", "+tnx", "+tnl", "+tno alice", "+tnk two,keys"])
@pytest.mark.asyncio
async def test_invalid_modes_are_refused_and_nothing_is_sent(docker_stack, fake_hub_p11,
                                                             tmp_path, modes):
    """A '-', an unknown mode, a missing argument, a member mode, a bad key."""
    hub = fake_hub_p11
    ts = int(time.time()) - 3600

    async with link_debug(hub, tmp_path, burst=_network_burst(hub, ts), gnutest=True,
                          burstchannel=f"{CHAN} {ts - 1000} {modes}") as (hub, proc):
        await proc.wait_for_stdout("): refused", timeout=10.0)
        assert _bursts_for(hub, CHAN) == []

        # The channel is as the network burst it
        info = await chaninfo(hub, await _asker(hub), CHAN)
        assert (info.created, info.modes, info.key) == (ts, "ikm", "oldkey")
        assert info.bans == {"*!*@banned.example.net"}


@pytest.mark.asyncio
async def test_a_newer_timestamp_is_refused(docker_stack, fake_hub_p11, tmp_path):
    hub = fake_hub_p11
    ts = int(time.time()) - 3600

    async with link_debug(hub, tmp_path, burst=_network_burst(hub, ts), gnutest=True,
                          burstchannel=f"{CHAN} {ts + 10} +tn") as (hub, proc):
        await proc.wait_for_stdout("): refused", timeout=10.0)
        assert _bursts_for(hub, CHAN) == []
        assert (await chaninfo(hub, await _asker(hub), CHAN)).created == ts
