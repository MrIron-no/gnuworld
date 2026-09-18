"""The channel modes u (no part messages), M (moderate the unauthenticated) and
Z (TLS only) came with P11. A P10 uplink does not know them: gnuworld neither sends them to
one nor believes them from one."""

from __future__ import annotations

import time

import pytest

import gnutest_client as gt
from debugquery import chaninfo
from test_inbound_modes import _expect_abort

PERMIT_ACCOUNT = "MrIron"
CHAN = "#proto"


async def _setup(hub):
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, PERMIT_ACCOUNT)
    alice = await hub.introduce_nick("alice", username="alice")
    ts = int(time.time()) - 3600
    await hub.send_raw(f"{hub.server_numnick} B {CHAN} {ts} +tn {alice}:o")
    return asker, alice, ts


@pytest.mark.asyncio
async def test_a_p11_link_has_all_three(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    asker, alice, ts = await _setup(hub)

    assert await gt.run(hub, asker, f"servmode {CHAN} +uMZ") == [f"{hub.peer_numeric} M {CHAN} +uMZ {ts}"]
    await hub.send_raw(f"{alice} M {CHAN} -u {ts}")
    assert (await chaninfo(hub, asker, CHAN)).modes == "MZnt"


@pytest.mark.asyncio
async def test_a_module_is_refused_them_on_a_p10_link(gnutest_linked_p10):
    hub, proc = gnutest_linked_p10
    asker, _alice, ts = await _setup(hub)

    # Refused as a whole, as any mode that cannot be set is: nothing is sent
    assert await gt.run(hub, asker, f"servmode {CHAN} +iM") == []
    await proc.wait_for_stdout("mode 'M' needs protocol 11")
    assert (await chaninfo(hub, asker, CHAN)).modes == "nt"

    # A join leaves them out and sets the rest
    assert await gt.run(hub, asker, f"servmode {CHAN} +Z") == []
    sent = await gt.run(hub, asker, "joinmodes #fresh +ntuMZ")
    assert [l for l in sent if " M #fresh " in l or " C #fresh" in l or " B #fresh" in l]
    assert not [l for l in sent if "u" in l.split(" ", 3)[-1].split(" ")[0] and " M " in l]
    assert (await chaninfo(hub, asker, "#fresh")).modes == "nt"


@pytest.mark.asyncio
async def test_a_p10_uplink_that_sends_one_cannot_be_parsed(debug_linked):
    hub, proc = debug_linked
    asker, alice, ts = await _setup(hub)

    line = f"{alice} M {CHAN} +M {ts}"
    await hub.send_raw(line)
    await _expect_abort(proc, line, "mode 'M' needs protocol 11")


@pytest.mark.asyncio
async def test_clearmode_names_no_letter_the_p10_uplink_lacks(gnutest_linked_p10):
    hub, _proc = gnutest_linked_p10
    asker, _alice, _ts = await _setup(hub)

    assert await gt.run(hub, asker, f"servclearmode {CHAN} ntuMZ") == [f"{hub.peer_numeric} CM {CHAN} :nt"]
