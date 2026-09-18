"""Inbound channel modes: MODE, OPMODE, CLEARMODE and the BURST mode block.

They share Channel::parseModes() and xServer::ApplyChannelModes(). A line
from the uplink that cannot be parsed is a protocol error: gnuworld says which
line, and why, and aborts, because its state is no longer the network's. A
change that parses but cannot be applied (+v for somebody who is gone) is a
race, not an error, and is skipped.
"""

from __future__ import annotations

import asyncio
import signal
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


async def _expect_abort(proc, line: str, problem: str) -> None:
    """gnuworld names the line and the problem, then dies of SIGABRT."""
    await proc.wait_for_stdout("PROTOCOL ERROR, cannot parse this line from the uplink: " + line)
    await proc.wait_for_stdout(problem)
    assert proc.proc is not None
    returncode = await asyncio.wait_for(proc.proc.wait(), timeout=10)
    # -SIGABRT when gnuworld is our child, 128 + SIGABRT through a shell
    assert returncode in (-signal.SIGABRT, 128 + signal.SIGABRT), returncode


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
async def test_a_change_that_cannot_be_applied_is_skipped(debug_linked_p11):
    hub, proc = debug_linked_p11
    asker, n, ts = await _setup(hub)

    # AzZZZ is nobody: he may have been killed by us while this was on its way
    await hub.send_raw(f"{n['opped']} M {CHAN} +i-t+kov sekrit AzZZZ {n['plain']} {ts}")
    info = await chaninfo(hub, asker, CHAN)
    assert info.modes == "ikn"
    assert info.key == "sekrit"
    assert info.members["plain"] == "+v"
    await proc.wait_for_stdout("no such member for mode 'o': AzZZZ")


UNPARSEABLE = [
    # line, with {c} the channel, {ts} its timestamp, {s} the hub and {u} an op
    ("{u} M {c} +ix {ts}", "unknown mode 'x'"),
    ("{u} M {c} +id {ts}", "mode 'd' is local to a server"),
    ("{u} M {c} +l many {ts}", "invalid limit for mode 'l': many"),
    ("{u} M {c} +k", "mode 'k' is missing its argument"),
    ("{u} M {c} +i stray {ts}", "unused argument: stray"),
    # not a timestamp, so it must not be read as one: 0 is the oldest there is
    ("{u} M {c} +m stray", "unused argument: stray"),
    ("{u} OM {c} +o", "mode 'o' is missing its argument"),
    ("{s} B #truncated {ts} +tl", "mode 'l' is missing its argument"),
    ("{s} B #keyed {ts} +tk", "mode 'k' is missing its argument"),
    ("{s} B #banned {ts} {u}:o :%*!*@a.example 1700000000", "not a multiple of 3"),
    ("{s} B #banned {ts} {u}:o :%*!*@a.example soon {u}", "invalid ban timestamp: soon"),
    ("{s} CM {c} ovx", "unknown mode: x"),
    ("{s} CM {c} bd", "mode is local to a server: d"),
]


@pytest.mark.asyncio
@pytest.mark.parametrize(("template", "problem"), UNPARSEABLE, ids=[line.format(c="#c", ts="ts", s="S", u="U") for line, _ in UNPARSEABLE])
async def test_a_line_that_cannot_be_parsed_aborts(debug_linked_p11, template, problem):
    hub, proc = debug_linked_p11
    asker, n, ts = await _setup(hub)
    line = template.format(c=CHAN, ts=ts, s=hub.server_numnick, u=n["opped"])
    await hub.send_raw(line)

    await _expect_abort(proc, line, problem)


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

    # Everything but +n and +t
    await hub.send_raw(f"{hub_yy} CM {CHAN} RmlkAUovb")
    after = await chaninfo(hub, asker, CHAN)
    assert after.modes == "nt"
    assert (after.key, after.limit) == (None, None)
    assert after.bans == set()
    assert after.members == {"plain": "none", "other": "none", "opped": "none"}
