"""SILENCE, the server's OPMODE on a user, a notice to everybody and a message
from the server: commands that more than one module sends, and so methods of
xClient and xServer. mod.gnutest calls them so that each is seen on the wire.

A command that only one module sends, that touches no state of ours and that
reads the same in P10 and P11, stays a Write() in that module: VERSION, TIME,
SETTIME and RPING of mod.ccontrol.
"""

from __future__ import annotations

import pytest

import gnutest_client as gt

PERMIT_ACCOUNT = "MrIron"


@pytest.mark.asyncio
async def test_commands_shared_between_modules(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, PERMIT_ACCOUNT)
    alice = await hub.introduce_nick("alice", username="alice")
    me, srv = gt.numnick(hub), hub.peer_numeric

    assert await gt.run(hub, asker, "silence alice *!*alice@*.example") == [
        f"{me} U {alice} *!*alice@*.example"
    ]
    assert await gt.run(hub, asker, "unsilence *!*alice@*.example") == [f"{me} U * -*!*alice@*.example"]

    assert await gt.run(hub, asker, "opmode alice +o") == [f"{srv} OM {alice} :+o"]
    assert await gt.run(hub, asker, "globalnotice back in five") == [f"{me} O $* :back in five"]
    assert await gt.run(hub, asker, "servsay alice hello") == [f"{srv} P {alice} :hello"]
