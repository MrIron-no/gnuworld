"""A timer runs the closure it was registered with, and nothing else.

xServer::RegisterTimer(time, owner, callback) is the whole timer API: the owner
is there only so that unloading one still cancels its timers, and what runs on
expiry is the closure.  Cancelling such a timer discards the closure and its
captures, and the owner is never told about a timer that went away.

mod.gnutest's "callbacktimer <seconds> <label>" registers one whose closure
reports its own label back through the event channel, so a test can tell which
closure ran; "callbacktimer cancel <id>" cancels one before it expires.

The other way a timer goes away is its owner being unloaded, which reaches
xServer::removeAllTimers().  Two of those here: "reload", which unloads and
reloads the instance while it is still reporting events, so what it is told
during the sweep is visible from the wire; and a detach from inside a handler,
with a timer close enough to expire afterwards, so that a timer the sweep
missed would have something of nobody's to run.
"""

from __future__ import annotations

import asyncio

import pytest

import fake_hub
import gnutest_client as gt
from p10 import p10_token, strip_msg_tags

# Far enough out that a timer registered for the unload test cannot expire
UNLOAD_SECONDS = 3600

# Close enough that a timer left behind by a detached owner would expire while
# the test is still watching
LEFT_BEHIND_SECONDS = 3


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


@pytest.mark.asyncio
async def test_unloading_the_owner_discards_a_callback_timer(gnutest_linked_p11):
    """One unload with a timer pending, and nothing is handed back for it.

    "reload" is xServer::UnloadClient() followed by LoadClient(), and the unload
    reaches unregisterClient() -> removeAllTimers(), the one path that cancels a
    module's timers for it. The module is still alive and still reporting events
    while that runs, so anything it were told there would be visible from the
    wire: there is nothing to tell it, and the closure's captures go with the
    closure. The timer cannot expire, so a CallbackTimer report would mean it
    survived its owner.
    """
    hub, proc = gnutest_linked_p11
    asker = await hub.introduce_nick("asker", username="asker")

    await gt.run(hub, asker, "events on")
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

    assert reported(hub, start, "CallbackTimer") == []


@pytest.mark.asyncio
async def test_a_timer_left_behind_by_a_detached_owner_never_runs(gnutest_linked_p11):
    """A timer whose owner is gone by the time it would have expired.

    Detaching from inside a handler is where a module leaving with timers still
    registered actually happens, and it is the far side of removeAllTimers()
    from the reload above: nothing comes back afterwards to say the sweep
    happened. What says it is the expiry itself passing with the module gone -
    a closure the sweep had missed would be run here, and it captured an
    xClient that has been deleted and a library that has been unmapped.
    """
    hub, proc = gnutest_linked_p11
    asker = await hub.introduce_nick("asker", username="asker")
    victim = await hub.introduce_nick("victim", username="victim")

    await gt.run(hub, asker, "events on")
    left = registered_id(await gt.run(hub, asker, f"callbacktimer {LEFT_BEHIND_SECONDS} orphan"))
    assert left != "0"

    await gt.run(hub, asker, "onevent Quit detachself")

    gone = gt.numnick(hub)
    start = len(hub.received)
    await hub.send_raw(f"{victim} Q :bye now")
    await hub.wait_for(lambda line: line.startswith(f"{gone} Q "), timeout=30.0)

    # Wait out the timer the departed instance registered
    await asyncio.sleep((LEFT_BEHIND_SECONDS + 3) * fake_hub.TIMEOUT_SCALE)

    assert proc.proc is not None and proc.proc.returncode is None, "gnuworld died"
    assert reported(hub, start, "CallbackTimer") == []
