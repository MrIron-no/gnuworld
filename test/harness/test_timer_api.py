"""What core hands a module when it destroys a timer the module never cancelled.

xServer::removeAllTimers() is the only place a TimerHandler is told about a
timer it did not unregister itself, so OnTimerDestroy(timerID, void*) is the one
part of the timer API that no module drives and no test reached: it was called
with the timer's expiry time where its id belongs, and with a pointer to core's
own timerInfo where the caller's own void* belongs, and nothing noticed.

mod.gnutest's "timer <seconds> <payload>" registers a timer far enough out that
it cannot fire, with a payload only gnutest can recognise; its OnTimerDestroy()
reports the id and the payload it was handed, and reports "?" for data that is
not one of its own rather than reading it. Detaching the module from inside a
handler is what runs removeAllTimers() for it.
"""

from __future__ import annotations

import pytest

import gnutest_client as gt
from p10 import strip_msg_tags

# Far enough out that the timer cannot expire during the test
SECONDS = 3600
PAYLOAD = "the-payload-gnutest-registered"


@pytest.mark.asyncio
async def test_a_destroyed_timer_is_reported_with_its_own_id_and_payload(gnutest_linked_p11):
    hub, proc = gnutest_linked_p11
    asker = await hub.introduce_nick("asker", username="asker")
    victim = await hub.introduce_nick("victim", username="victim")

    await gt.run(hub, asker, "events on")
    out = await gt.run(hub, asker, f"timer {SECONDS} {PAYLOAD}")
    (registered,) = [
        line.split(" :Timer ", 1)[1].split(" ")[0] for line in out if " :Timer " in line
    ]

    # The detach runs from inside a handler, which is where a module leaving
    # with timers still registered actually happens
    await gt.run(hub, asker, "onevent Quit detachself")

    start = len(hub.received)
    await hub.send_raw(f"{victim} Q :bye now")
    await hub.wait_for(lambda line: " :EVENT TimerDestroy " in line, timeout=30.0)
    assert proc.proc is not None and proc.proc.returncode is None, "gnuworld died"

    reported = [
        strip_msg_tags(line).split(" :EVENT ", 1)[1]
        for line in hub.received[start:]
        if " :EVENT TimerDestroy " in strip_msg_tags(line)
    ]
    assert reported == [f"TimerDestroy {registered} {PAYLOAD}"]
