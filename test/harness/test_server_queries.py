"""What a client of ours asks of, and orders, a server: VERSION, TIME, SETTIME,
RPING, SILENCE, and the server's OPMODE on a user.

mod.ccontrol wrote these lines itself. They are methods of xClient and xServer
now, and mod.gnutest calls them so that each can be seen on the wire.
"""

from __future__ import annotations

import re
import time

import pytest

import gnutest_client as gt

PERMIT_ACCOUNT = "MrIron"


@pytest.mark.asyncio
async def test_questions_and_orders_for_a_server(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, PERMIT_ACCOUNT)
    alice = await hub.introduce_nick("alice", username="alice")
    me, srv, hub_yy = gt.numnick(hub), hub.peer_numeric, hub.server_numnick
    hub_name = hub.name

    assert await gt.run(hub, asker, f"version {hub_name}") == [f"{me} V :{hub_yy}"]
    assert await gt.run(hub, asker, f"time {hub_name}") == [f"{me} TI :{hub_yy}"]

    (settime,) = await gt.run(hub, asker, f"settime {hub_name}")
    match = re.fullmatch(rf"{me} SE (\d+) {hub_yy}", settime)
    assert match and abs(int(match.group(1)) - time.time()) < 300, settime

    # <our server> RI <server> <requester> <sec> <usec> :<sec> <usec>
    (rping,) = await gt.run(hub, asker, f"rping {hub_name}")
    match = re.fullmatch(rf"{srv} RI {hub_yy} {me} (\d+) (\d+) :(\d+) (\d+)", rping)
    assert match and match.group(1, 2) == match.group(3, 4), rping
    assert abs(int(match.group(1)) - time.time()) < 300

    assert await gt.run(hub, asker, "silence alice *!*alice@*.example") == [
        f"{me} U {alice} *!*alice@*.example"
    ]
    assert await gt.run(hub, asker, "unsilence *!*alice@*.example") == [f"{me} U * -*!*alice@*.example"]

    assert await gt.run(hub, asker, "opmode alice +o") == [f"{srv} OM {alice} :+o"]
    assert await gt.run(hub, asker, "globalnotice back in five") == [f"{me} O $* :back in five"]
    assert await gt.run(hub, asker, "servsay alice hello") == [f"{srv} P {alice} :hello"]
