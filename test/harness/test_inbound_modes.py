"""Inbound channel modes: what the shared parser and applier fixed.

MODE, OPMODE and the BURST mode block used to have a letter switch each. They
now go through libgnuworld/ChannelModes (parse) and src/ChannelModeApply. These
cases are the defects the old handlers had, so they stay fixed.
"""

from __future__ import annotations

import time

import pytest

from debugquery import chaninfo

PERMIT_ACCOUNT = "MrIron"
CHAN = "#inbound"


async def _setup(hub) -> tuple[str, dict[str, str], int]:
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, PERMIT_ACCOUNT)
    n = {nick: await hub.introduce_nick(nick, username=nick) for nick in ("plain", "other", "opped")}
    ts = int(time.time()) - 3600
    await hub.send_raw(
        f"{hub.server_numnick} B {CHAN} {ts} +tn {n['plain']},{n['other']},{n['opped']}:o"
    )
    return asker, n, ts


@pytest.mark.asyncio
async def test_stray_argument_does_not_reset_the_creation_time(debug_linked_p11):
    """The old handler ran atoi() over any leftover argument and took the result,
    0, as an older channel timestamp."""
    hub, _proc = debug_linked_p11
    asker, n, ts = await _setup(hub)

    await hub.send_raw(f"{n['opped']} M {CHAN} +m stray")
    info = await chaninfo(hub, asker, CHAN)
    assert info.created == ts
    assert "m" in info.modes  # the valid part of the line is still applied


@pytest.mark.asyncio
async def test_admin_and_user_pass_are_modes_with_an_argument(debug_linked_p11):
    """+A/+U were unknown to the MODE handler, so their argument was never
    consumed and ended up as the "timestamp", zeroing the creation time."""
    hub, _proc = debug_linked_p11
    asker, n, ts = await _setup(hub)

    await hub.send_raw(f"{n['opped']} M {CHAN} +AU apass upass {ts}")
    info = await chaninfo(hub, asker, CHAN)
    assert info.created == ts
    assert {"A", "U"} <= set(info.modes)


@pytest.mark.asyncio
async def test_timestamp_is_adopted_only_when_older(debug_linked_p11):
    hub, _proc = debug_linked_p11
    asker, n, ts = await _setup(hub)

    await hub.send_raw(f"{n['opped']} M {CHAN} +m {ts + 500}")
    assert (await chaninfo(hub, asker, CHAN)).created == ts

    await hub.send_raw(f"{n['opped']} M {CHAN} +i {ts - 500}")
    assert (await chaninfo(hub, asker, CHAN)).created == ts - 500

    # "+l 10" with no timestamp behind it: 10 is the limit, not the timestamp
    await hub.send_raw(f"{n['opped']} M {CHAN} +l 10")
    info = await chaninfo(hub, asker, CHAN)
    assert (info.limit, info.created) == (10, ts - 500)


@pytest.mark.asyncio
async def test_op_with_an_oplevel_suffix(debug_linked_p11):
    """With OPLEVELS the target is "<numeric>:<level>"; the lookup did not strip
    the level, so the op was lost."""
    hub, _proc = debug_linked_p11
    asker, n, ts = await _setup(hub)

    await hub.send_raw(f"{n['opped']} M {CHAN} +ov {n['plain']}:999 {n['other']} {ts}")
    members = (await chaninfo(hub, asker, CHAN)).members
    assert (members["plain"], members["other"]) == ("+o", "+v")


@pytest.mark.asyncio
async def test_bad_modes_are_skipped_and_the_rest_applied(debug_linked_p11):
    hub, _proc = debug_linked_p11
    asker, n, ts = await _setup(hub)

    # 'd' is local to an ircu server, 'x' does not exist, the limit is not a
    # number and AzZZZ is nobody. +i, -t, +k and +v must still go through, and
    # the bad limit must not shift the key onto the wrong argument.
    await hub.send_raw(
        f"{n['opped']} M {CHAN} +idx-t+lkov many sekrit AzZZZ {n['plain']} {ts}"
    )
    info = await chaninfo(hub, asker, CHAN)
    assert info.modes == "ikn"
    assert (info.key, info.limit) == ("sekrit", None)
    assert info.members["plain"] == "+v"


@pytest.mark.asyncio
async def test_truncated_burst_mode_block(debug_linked_p11):
    """"+l" with nothing behind it made the BURST handler read past the end of
    its parameters."""
    hub, proc = debug_linked_p11
    asker, n, ts = await _setup(hub)

    await hub.send_raw(f"{hub.server_numnick} B #truncated {ts} +tl")
    await hub.send_raw(f"{hub.server_numnick} B #keyed {ts} +tk")

    # Still alive and answering, and the valid flag of each block was applied
    assert (await chaninfo(hub, asker, "#truncated")).modes == "t"
    assert (await chaninfo(hub, asker, "#keyed")).modes == "t"
    assert proc.proc is not None and proc.proc.returncode is None


@pytest.mark.asyncio
async def test_opmode_from_a_client_that_is_not_on_the_channel(debug_linked_p11):
    hub, _proc = debug_linked_p11
    asker, n, ts = await _setup(hub)
    oper = await hub.introduce_nick("oper", username="oper", modes="+io")

    await hub.send_raw(f"{oper} OM {CHAN} +o {n['plain']} {ts}")
    assert (await chaninfo(hub, asker, CHAN)).members["plain"] == "+o"


@pytest.mark.asyncio
async def test_clearmode_covers_every_kind_of_mode(debug_linked_p11):
    """CLEARMODE had its own letter switch, which did not know R, A or U."""
    hub, _proc = debug_linked_p11
    asker, n, ts = await _setup(hub)
    hub_yy = hub.server_numnick

    await hub.send_raw(f"{hub_yy} M {CHAN} +RmlkAU 5 sekrit apass upass {ts}")
    await hub.send_raw(f"{n['opped']} M {CHAN} +vb {n['plain']} *!*@spam.example.net {ts}")
    before = await chaninfo(hub, asker, CHAN)
    assert before.modes == "".join(sorted("AURklmnt"))
    assert before.bans == {"*!*@spam.example.net"}

    # Everything but +n and +t; 'd' is server-local and 'x' unknown: both ignored
    await hub.send_raw(f"{hub_yy} CM {CHAN} RmlkAUovbdx")
    after = await chaninfo(hub, asker, CHAN)
    assert after.modes == "nt"
    assert (after.key, after.limit) == (None, None)
    assert after.bans == set()
    assert after.members == {"plain": "none", "other": "none", "opped": "none"}
