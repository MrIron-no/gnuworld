"""A timer registered with a closure runs that closure, and nothing else.

xServer::RegisterTimer(time, owner, callback) is the timer API modules are
moving to: the owner is there only so that unloading one still cancels its
timers, and what runs on expiry is the closure, not the owner's OnTimer().
Cancelling such a timer discards the closure, and no OnTimerDestroy() is sent
for it.

mod.gnutest's "callbacktimer <seconds> <label>" registers one whose closure
reports its own label back through the event channel, so a test can tell which
closure ran; "callbacktimer cancel <id>" cancels one before it expires.
"""

from __future__ import annotations

import pytest

import gnutest_client as gt
from p10 import strip_msg_tags


def registered_id(lines: list[str]) -> str:
    """The timerID gnutest reported for a "callbacktimer" registration."""
    ids = [
        line.split(" :Callback timer ", 1)[1].split(" ")[0]
        for line in lines
        if " :Callback timer " in line and line.rstrip().endswith("registered")
    ]
    assert len(ids) == 1, f"expected one registration, got {ids} from {lines}"
    return ids[0]


def reported(hub, start: int, name: str) -> list[str]:
    """Every "EVENT <name> ..." gnutest sent since ``start``."""
    return [
        strip_msg_tags(line).split(" :EVENT ", 1)[1]
        for line in hub.received[start:]
        if f" :EVENT {name} " in strip_msg_tags(line)
    ]


@pytest.mark.asyncio
async def test_a_callback_timer_runs_its_own_closure(gnutest_linked_p11):
    hub, proc = gnutest_linked_p11
    asker = await hub.introduce_nick("asker", username="asker")

    await gt.run(hub, asker, "events on")

    start = len(hub.received)
    out = await gt.run(hub, asker, "callbacktimer 2 first-closure")
    assert registered_id(out) != "0"

    await hub.wait_for(lambda line: " :EVENT CallbackTimer " in line, timeout=30.0)
    assert proc.proc is not None and proc.proc.returncode is None, "gnuworld died"
    assert reported(hub, start, "CallbackTimer") == ["CallbackTimer first-closure"]


@pytest.mark.asyncio
async def test_a_cancelled_callback_timer_never_runs(gnutest_linked_p11):
    hub, proc = gnutest_linked_p11
    asker = await hub.introduce_nick("asker", username="asker")

    await gt.run(hub, asker, "events on")

    start = len(hub.received)
    # The cancelled one expires first, so by the time the kept one has run it
    # would have run too
    doomed = registered_id(await gt.run(hub, asker, "callbacktimer 2 cancelled-closure"))
    await gt.run(hub, asker, "callbacktimer 5 kept-closure")

    out = await gt.run(hub, asker, f"callbacktimer cancel {doomed}")
    assert [line for line in out if " :Callback timer " in line and "cancelled" in line], out

    await hub.wait_for(lambda line: " :EVENT CallbackTimer " in line, timeout=30.0)
    assert proc.proc is not None and proc.proc.returncode is None, "gnuworld died"
    assert reported(hub, start, "CallbackTimer") == ["CallbackTimer kept-closure"]

    # A callback timer that goes away has no OnTimerDestroy(): its closure is
    # simply discarded
    assert reported(hub, start, "TimerDestroy") == []
