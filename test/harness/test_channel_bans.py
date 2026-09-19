"""Who set each ban and when, as mod.debug CHANINFO reports it.

P10 carries neither with a ban, so what the core records is what it saw: the
source of a live MODE +b, and for a burst ban what the burst says - a P11 ban
is a "mask timestamp setter" triplet - or else the server the BURST came from,
which is what ircu does with a P10 one.

CHANINFO prints one ban per notice now, "<mask>  set by <who>  at <when>";
debugquery.chaninfo() reads the setter and the time back beside the mask.
"""

from __future__ import annotations

import time

import pytest

from conftest import link_debug
from debugquery import chaninfo

PERMIT_ACCOUNT = "MrIron"
CHAN = "#bans"
NARROW = "*!*@sub.bad.host"
WIDE = "*!*@*.bad.host"


async def _asker(hub) -> str:
    """A client mod.debug will answer."""
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, PERMIT_ACCOUNT)
    return asker


async def _burst_channel(hub, ts: int, bans: str = "") -> str:
    """Burst CHAN with alice opped, and whatever ban section is given.
    Returns alice's numnick."""
    alice = await hub.introduce_nick("alice", username="alice", host="alice.example.net")
    await hub.send_raw(f"{hub.server_numnick} B {CHAN} {ts} +tn {alice}:o{bans}")
    return alice


def _plausible(when: int, around: float) -> bool:
    """A time the core took from its own clock, not from the wire."""
    return abs(when - around) <= 30


@pytest.mark.asyncio
async def test_a_ban_a_user_sets_is_recorded_as_that_users(docker_stack, fake_hub_p11, tmp_path):
    ts = int(time.time()) - 3600

    async with link_debug(fake_hub_p11, tmp_path) as (hub, _proc):
        asker = await _asker(hub)
        alice = await _burst_channel(hub, ts)

        before = time.time()
        await hub.send_raw(f"{alice} M {CHAN} +b {NARROW} {ts}")

        info = await chaninfo(hub, asker, CHAN)
        assert info.bans == {NARROW}
        assert info.ban_setters == {NARROW: "alice"}  # the nick, as ircu stores it
        assert _plausible(info.ban_times[NARROW], before)


@pytest.mark.asyncio
async def test_a_ban_a_server_sets_is_recorded_as_that_servers(docker_stack, fake_hub_p11,
                                                               tmp_path):
    """An OPMODE or a server MODE has no member behind it, only a name."""
    ts = int(time.time()) - 3600

    async with link_debug(fake_hub_p11, tmp_path) as (hub, _proc):
        asker = await _asker(hub)
        await _burst_channel(hub, ts)

        before = time.time()
        await hub.send_raw(f"{hub.server_numnick} M {CHAN} +b {NARROW} {ts}")

        info = await chaninfo(hub, asker, CHAN)
        assert info.ban_setters == {NARROW: hub.name}
        assert _plausible(info.ban_times[NARROW], before)


@pytest.mark.asyncio
async def test_a_p11_burst_ban_keeps_what_the_triplet_says(docker_stack, fake_hub_p11, tmp_path):
    """"mask timestamp setter": the burst knows both, so neither is guessed."""
    ts = int(time.time()) - 3600
    banTS = ts + 100

    async with link_debug(fake_hub_p11, tmp_path) as (hub, _proc):
        asker = await _asker(hub)
        await _burst_channel(hub, ts, bans=f" :%{NARROW} {banTS} OldOp")

        info = await chaninfo(hub, asker, CHAN)
        assert info.bans == {NARROW}
        assert info.ban_setters == {NARROW: "OldOp"}
        assert info.ban_times == {NARROW: banTS}  # the wire's time, not ours


@pytest.mark.asyncio
async def test_what_a_p11_triplet_says_is_shown_safely(docker_stack, fake_hub_p11, tmp_path):
    """The setter and the time are another server's words: a control character
    of the name stays out of the notice, and a time ahead of ours is still a
    line that can be read."""
    ts = int(time.time()) - 3600
    banTS = int(time.time()) + 86400

    async with link_debug(fake_hub_p11, tmp_path) as (hub, _proc):
        asker = await _asker(hub)
        await _burst_channel(hub, ts, bans=f" :%{NARROW} {banTS} Old\x02Op")

        info = await chaninfo(hub, asker, CHAN)
        assert info.ban_setters == {NARROW: "OldOp"}
        assert info.ban_times == {NARROW: banTS}


@pytest.mark.asyncio
async def test_a_p11_ban_time_that_is_no_number_is_a_warning(docker_stack, fake_hub_p11, tmp_path):
    """The time of a ban is somebody's note, passed on by every server since:
    one that cannot be read costs a warning, and neither the link nor the ban."""
    ts = int(time.time()) - 3600

    async with link_debug(fake_hub_p11, tmp_path) as (hub, proc):
        asker = await _asker(hub)
        before = int(time.time())
        await _burst_channel(hub, ts, bans=f" :%{NARROW} soon OldOp")

        info = await chaninfo(hub, asker, CHAN)
        assert info.bans == {NARROW}
        assert info.ban_setters == {NARROW: "OldOp"}
        assert before - 2 <= info.ban_times[NARROW] <= int(time.time()) + 2  # when we learnt of it

        await proc.wait_for_stdout("has a timestamp that is no number: soon", timeout=10.0)


@pytest.mark.asyncio
async def test_a_p10_burst_ban_is_the_bursting_servers(docker_stack, fake_hub, tmp_path):
    """A P10 burst is a flat list of masks: nobody to name but the sender."""
    ts = int(time.time()) - 3600

    async with link_debug(fake_hub, tmp_path) as (hub, _proc):
        asker = await _asker(hub)
        before = time.time()
        await _burst_channel(hub, ts, bans=f" :%{NARROW}")

        info = await chaninfo(hub, asker, CHAN)
        assert info.bans == {NARROW}
        assert info.ban_setters == {NARROW: hub.name}
        assert _plausible(info.ban_times[NARROW], before)


@pytest.mark.asyncio
async def test_a_removed_ban_is_gone_from_the_output(docker_stack, fake_hub_p11, tmp_path):
    ts = int(time.time()) - 3600

    async with link_debug(fake_hub_p11, tmp_path) as (hub, _proc):
        asker = await _asker(hub)
        alice = await _burst_channel(hub, ts, bans=f" :%{NARROW} {ts} OldOp")
        assert (await chaninfo(hub, asker, CHAN)).bans == {NARROW}

        await hub.send_raw(f"{alice} M {CHAN} -b {NARROW} {ts}")

        info = await chaninfo(hub, asker, CHAN)
        assert info.found
        assert info.bans == set()
        assert info.ban_setters == {}


@pytest.mark.asyncio
async def test_a_wider_ban_takes_over_from_the_one_it_overlaps(docker_stack, fake_hub_p11,
                                                              tmp_path):
    """The narrow ban's details go with it: only the new one is reported."""
    ts = int(time.time()) - 3600

    async with link_debug(fake_hub_p11, tmp_path) as (hub, _proc):
        asker = await _asker(hub)
        alice = await _burst_channel(hub, ts, bans=f" :%{NARROW} {ts} OldOp")

        before = time.time()
        await hub.send_raw(f"{alice} M {CHAN} +b {WIDE} {ts}")

        info = await chaninfo(hub, asker, CHAN)
        assert info.bans == {WIDE}
        assert info.ban_setters == {WIDE: "alice"}
        assert _plausible(info.ban_times[WIDE], before)
