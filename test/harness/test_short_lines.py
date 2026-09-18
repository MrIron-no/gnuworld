"""A line from the uplink with fewer parameters than the shortest form of its
command cannot be parsed: a protocol error, and gnuworld aborts.

The minimum for each command is the one ircu's own server handler has (the
"parc <" check of its ms_*() function), so that a line ircu takes is never one
gnuworld dies of. Some commands have a legal form that is shorter than the one
gnuworld can use; that one is left alone, and gnuworld carries on.
"""

from __future__ import annotations

import asyncio
import signal
import time

import pytest

from debugquery import chaninfo
from test_inbound_modes import _expect_abort, _setup, CHAN

TOO_SHORT = [
    # line, with {s} the hub, {u} a client and {c} a channel; what it has, and what is needed
    ("{s} AC {u}", 2, 3),
    ("{s} B {c}", 2, 3),
    ("{u} C {c}", 2, 3),
    ("{s} CF 1700000000", 2, 3),
    ("{s} CM {c}", 2, 3),
    ("{s} D {u}", 2, 3),
    ("{s} G", 1, 2),
    ("{s} GL *", 2, 3),
    ("{u} I asker", 2, 3),
    ("{u} J", 1, 2),
    ("{s} JU * +juped.testnet 3600 1700000000", 5, 6),
    ("{u} K {c}", 2, 3),
    ("{u} L", 1, 2),
    ("{u} M {c}", 2, 3),
    ("{u} N", 1, 3),
    ("{u} O asker", 2, 3),
    ("{u} P asker", 2, 3),
    ("{s} RI {s}", 2, 3),
    ("{s} RO", 1, 3),
    ("{u} RV {c}", 2, 3),
    ("{s} S leaf.testnet 2 0 1700000000 P11 ACAAB", 7, 8),
    ("{s} SQ", 1, 2),
    ("{u} T {c}", 2, 3),
    ("{u} V", 1, 2),
    ("{u} W", 1, 2),
    ("{s} XQ {s} routing", 3, 4),
    ("{s} XR {s} routing", 3, 4),
]


@pytest.mark.asyncio
@pytest.mark.parametrize(("template", "has", "needs"), TOO_SHORT,
                         ids=[line.split(" ")[1] for line, _, _ in TOO_SHORT])
async def test_a_line_shorter_than_any_form_of_its_command_aborts(debug_linked_p11, template, has, needs):
    hub, proc = debug_linked_p11
    _asker, n, _ts = await _setup(hub)

    line = template.format(s=hub.server_numnick, u=n["opped"], c=CHAN)
    await hub.send_raw(line)
    await _expect_abort(proc, line, f"{has} parameters, where the shortest form has {needs}")


@pytest.mark.asyncio
async def test_a_legal_form_that_is_of_no_use_to_us_is_left_alone(debug_linked_p11):
    hub, proc = debug_linked_p11
    asker, n, ts = await _setup(hub)
    s, u, now = hub.server_numnick, n["opped"], int(time.time())

    for line in (
        f"{s} GL * +*@bad.example {now}",        # activation of a G-line the sender has not got either
        f"{s} GL * -*@bad.example",              # removal, in the old form
        f"{u} RI {hub.peer_numeric} {hub.peer_name} :an oper's RPING",
        f"{u} W :somebody",                      # a WHOIS that names no server
        f"{u} J #nots",                          # a JOIN without a timestamp
        f"{u} T {CHAN} :",                       # the topic cleared: an empty last parameter is one
        f"{s} B #zannel {ts}",                   # a burst of a channel with nothing but its timestamp
        f"{u} Q",                                # a QUIT without a reason
    ):
        await hub.send_raw(line)

    # The quitter is gone; everything else left gnuworld where it was
    info = await chaninfo(hub, asker, CHAN)
    assert "opped" not in info.members and info.topic == ""
    assert proc.proc is not None and proc.proc.returncode is None


@pytest.mark.asyncio
async def test_a_handler_that_reads_past_the_end_aborts_as_well(debug_linked_p11):
    """The backstop: xParameters::operator[] past the end names the line and
    aborts, in every build. A server's NICK has at least eight parameters;
    this one passes for a nick change by its count, and is not one."""
    hub, proc = debug_linked_p11
    await _setup(hub)

    await hub.send_raw(f"{hub.server_numnick} N fresh 1 {int(time.time())} fresh")
    await proc.wait_for_stdout("PROTOCOL ERROR")
    assert proc.proc is not None
    returncode = await asyncio.wait_for(proc.proc.wait(), timeout=10)
    assert returncode in (-signal.SIGABRT, 128 + signal.SIGABRT), returncode
