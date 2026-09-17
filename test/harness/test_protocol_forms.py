"""Lines whose layout depends on the protocol the uplink announced.

gnuworld reads the protocol from the uplink's SERVER line ("J10" or "J11") and
sends what that uplink understands. The P11 forms are asserted throughout the
other outbound tests; these are the P10 ones.
"""

from __future__ import annotations

import time

import pytest

import gnutest_client as gt

CHAN = "#p10"


@pytest.mark.asyncio
async def test_topic_and_invite_on_a_p10_uplink(gnutest_linked_p10):
    hub, _proc = gnutest_linked_p10
    assert hub.protocol == 10

    asker = await hub.introduce_nick("asker", username="asker")
    alice = await hub.introduce_nick("alice", username="alice")
    ts = int(time.time()) - 3600
    await hub.send_raw(f"{hub.server_numnick} B {CHAN} {ts} +n {alice}:o")
    me = gt.numnick(hub)

    assert await gt.run(hub, asker, f"join {CHAN}") == [f"{me} J {CHAN} {ts}"]

    # No timestamps in a P10 TOPIC, and the invitee of an INVITE goes by nick
    assert await gt.run(hub, asker, f"topic {CHAN} hello there") == [f"{me} T {CHAN} :hello there"]
    assert await gt.run(hub, asker, f"invite {CHAN}") == [f"{me} I asker {CHAN}"]

    # A channel MODE ends in the channel timestamp on either protocol: P10
    # takes it as optional, P11 requires it
    assert await gt.run(hub, asker, f"servmode {CHAN} +m") == [f"{hub.peer_numeric} M {CHAN} +m {ts}"]
