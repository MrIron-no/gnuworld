"""A timer registered with a closure runs that closure, and nothing else.

xServer::RegisterTimer(time, owner, callback) is the timer API modules are
moving to: the owner is there only so that unloading one still cancels its
timers, and what runs on expiry is the closure, not the owner's OnTimer().
Cancelling such a timer discards the closure, and no OnTimerDestroy() is sent
for it.

mod.gnutest's "callbacktimer <seconds> <label>" registers one whose closure
reports its own label back through the event channel, so a test can tell which
closure ran; "callbacktimer cancel <id>" cancels one before it expires.

The other way a timer goes away is its owner being unloaded, which reaches
xServer::removeAllTimers(): a legacy timer is handed back to the module through
OnTimerDestroy() there, and a callback timer is not. mod.gnutest's "reload"
unloads and reloads that instance, so one reload with both kinds of timer
pending shows both halves of that asymmetry.
"""

from __future__ import annotations

import pytest

import gnutest_client as gt
from p10 import p10_token, strip_msg_tags

# Far enough out that a timer registered for the unload test cannot expire
UNLOAD_SECONDS = 3600
PAYLOAD = "the-payload-gnutest-registered"


def registered_id(lines: list[str]) -> str:
    """The timerID gnutest reported for a "callbacktimer" registration."""
    ids = [
        line.split(" :Callback timer ", 1)[1].split(" ")[0]
        for line in lines
        if " :Callback timer " in line and line.rstrip().endswith("registered")
    ]
    assert len(ids) == 1, f"expected one registration, got {ids} from {lines}"
    return ids[0]


def legacy_id(lines: list[str]) -> str:
    """The timerID gnutest reported for a legacy "timer" registration."""
    ids = [
        line.split(" :Timer ", 1)[1].split(" ")[0]
        for line in lines
        if " :Timer " in line and line.rstrip().endswith("registered")
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


@pytest.mark.asyncio
async def test_unloading_the_owner_discards_a_callback_timer_and_hands_back_a_legacy_one(
    gnutest_linked_p11,
):
    """One unload, two pending timers, and only the legacy one is handed back.

    "reload" is xServer::UnloadClient() followed by LoadClient(), and the unload
    reaches unregisterClient() -> removeAllTimers(), the one path that cancels a
    module's timers for it. The module is still alive and still reporting events
    while that runs, so what it is told there is visible from the wire: an
    OnTimerDestroy() for the void* timer, and nothing at all for the closure,
    whose captures go with it. Neither timer can expire, so a CallbackTimer
    report would mean the closure survived its owner.
    """
    hub, proc = gnutest_linked_p11
    asker = await hub.introduce_nick("asker", username="asker")

    await gt.run(hub, asker, "events on")
    legacy = legacy_id(await gt.run(hub, asker, f"timer {UNLOAD_SECONDS} {PAYLOAD}"))
    callback = registered_id(await gt.run(hub, asker, f"callbacktimer {UNLOAD_SECONDS} unloaded"))
    assert callback != "0"

    gone = gt.numnick(hub)
    start = len(hub.received)
    await hub.send_privmsg(asker, gone, "reload")
    await hub.wait_for(lambda line: line.startswith(f"{gone} Q "), timeout=30.0)

    # LoadClient() is a timer two seconds out, so the fresh introduction is both
    # the proof that the unload ran to completion and the sentinel that anything
    # the outgoing instance had to say has been received by now
    await hub.wait_for(
        lambda line: p10_token(line) == "N" and " gnutest " in line,
        timeout=60.0,
        after=start,
    )
    assert proc.proc is not None and proc.proc.returncode is None, "gnuworld died"

    assert reported(hub, start, "TimerDestroy") == [f"TimerDestroy {legacy} {PAYLOAD}"]
    assert reported(hub, start, "CallbackTimer") == []
