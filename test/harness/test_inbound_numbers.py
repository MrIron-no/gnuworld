"""Numbers the uplink writes into a line: timestamps, the account id.

ircu formats these itself, so one that is not a number cannot be parsed:
xServer::RequireNumber() makes it a protocol error and gnuworld aborts, as it
does for a mode it cannot parse (test_inbound_modes.py). atoi() used to turn
such a field into 0, which as a timestamp is the oldest there is and wins every
conflict.
"""

from __future__ import annotations

import time

import pytest

from debugquery import chaninfo
from test_inbound_modes import _expect_abort, _setup, CHAN

NOT_A_NUMBER = [
    # line, with {c} a channel that exists, {ts} its timestamp, {s} the hub,
    # {u} a client on it and {now} the time
    ("{s} B #new soon +tn {u}:o", "invalid channel timestamp: soon"),
    ("{s} B {c} 12x {u}:o", "invalid channel timestamp: 12x"),
    ("{u} J #other -5", "invalid channel timestamp: -5"),
    ("{u} C #other now", "invalid channel timestamp: now"),
    ("{u} T {c} {ts} later :a topic", "invalid topic timestamp: later"),
    ("{u} RV {c} never", "invalid channel timestamp: never"),
    ("{u} N renamed 0x10", "invalid nick timestamp: 0x10"),
    ("{s} N fresh 1 then fresh fake.testnet +i B]AAAB ABAAX :Fake User", "invalid nick timestamp: then"),
    ("{s} N fresh 1 {now} fresh fake.testnet +ir acc:id B]AAAB ABAAX :Fake", "invalid account id: id"),
    ("{s} N fresh 1 {now} fresh fake.testnet +ir acc:7:f B]AAAB ABAAX :Fake", "invalid account flags: f"),
    ("{s} AC {u} someone seven", "invalid account id: seven"),
    ("{s} AC {u} someone 7 flags", "invalid account flags: flags"),
    ("{s} S leaf.testnet 2 0 yesterday P11 ACAAB +s6 :A leaf", "invalid link time: yesterday"),
    ("{s} GL * +*@bad.example soon {now} {now} :a reason", "invalid expire time: soon"),
    ("{s} GL * +*@bad.example 3600 then {now} :a reason", "invalid lastmod: then"),
    ("{s} JU * +juped.testnet 3600 then :a reason", "invalid lastmod: then"),
    ("{s} CF when some.key :a value", "invalid timestamp: when"),
]


def _fill(template: str, hub, n: dict[str, str], ts: int) -> str:
    return template.format(c=CHAN, ts=ts, s=hub.server_numnick, u=n["opped"], now=int(time.time()))


@pytest.mark.asyncio
@pytest.mark.parametrize(("template", "problem"), NOT_A_NUMBER, ids=[line.split(" ")[1] + ": " + p for line, p in NOT_A_NUMBER])
async def test_a_number_that_is_not_one_aborts(debug_linked_p11, template, problem):
    hub, proc = debug_linked_p11
    _asker, n, ts = await _setup(hub)

    line = _fill(template, hub, n, ts)
    await hub.send_raw(line)
    await _expect_abort(proc, line, problem)


@pytest.mark.asyncio
async def test_the_same_lines_with_numbers_are_fine(debug_linked_p11):
    hub, proc = debug_linked_p11
    asker, n, ts = await _setup(hub)
    now = int(time.time())

    for template, _problem in NOT_A_NUMBER:
        line = _fill(template, hub, n, ts)
        for bad in ("soon", "12x", "-5", "now", "later", "never", "0x10", "then", "yesterday", "when"):
            line = line.replace(f" {bad} ", f" {now} ").removesuffix(f" {bad}") + (
                f" {now}" if line.endswith(f" {bad}") else ""
            )
        line = line.replace("acc:id", "acc:7").replace("acc:7:f", "acc:7:3")
        line = line.replace("someone seven", "someone 7").replace("someone 7 flags", "someone 7 3")
        # the two N lines introduce the same client
        line = line.replace("ABAAX", "ABAAY") if "acc:7:3" in line else line
        line = line.replace(" fresh ", " fresh2 ", 1) if "acc:7:3" in line else line
        await hub.send_raw(line)

    # Still there, and the channel is as the lines left it
    info = await chaninfo(hub, asker, CHAN)
    assert info.topic == "a topic"
    assert proc.proc is not None and proc.proc.returncode is None

    # A GLINE from a server that does not send lastmod: the reason is not one
    await hub.send_raw(f"{hub.server_numnick} GL * +*@old.example 3600 :an old style reason")
    await chaninfo(hub, asker, CHAN)
    assert proc.proc.returncode is None


@pytest.mark.asyncio
async def test_an_account_id_the_login_server_wrote_is_not_ours_to_abort_for(debug_linked_p11):
    """"name:id" inside the account of an AC is passed on by ircu as it came."""
    hub, proc = debug_linked_p11
    asker, n, _ts = await _setup(hub)

    await hub.send_raw(f"{hub.server_numnick} AC {n['plain']} someone:notanumber")
    await proc.wait_for_stdout("msg_AC> Invalid account id: notanumber")
    await chaninfo(hub, asker, CHAN)
    assert proc.proc is not None and proc.proc.returncode is None
