"""What mod.ccontrol sends for the oper commands that change a channel or speak.

Recorded before these commands were moved from raw Write()s to the core API,
so that the move can be seen to leave the wire as it was, or to correct it.
"""

from __future__ import annotations

import time

import pytest

import ccontrol_client as cc
from p10 import p10_token

CHAN = "#opers"


async def _setup(hub):
    oper = await cc.login(hub)
    n = {nick: await hub.introduce_nick(nick, username=nick, host=f"{nick}.example.net")
         for nick in ("alice", "bob")}
    ts = int(time.time()) - 3600
    await hub.send_raw(f"{hub.server_numnick} B {CHAN} {ts} +tn {n['bob']},{n['alice']}:o")
    return oper, n, ts


@pytest.mark.asyncio
async def test_channel_commands(ccontrol_linked):
    hub, _proc = ccontrol_linked
    oper, n, ts = await _setup(hub)
    srv, bot = hub.peer_numeric, cc.numnick(hub)

    async def sent(command: str) -> list[str]:
        return cc.network(await cc.run(hub, oper, command), oper)

    assert await sent(f"mode {CHAN} +im") == [f"{srv} M {CHAN} +im {ts}"]
    assert await sent(f"mode {CHAN} +b *!*@spam.example") == [f"{srv} M {CHAN} +b *!*@spam.example {ts}"]
    assert await sent(f"mode {CHAN} +o bob") == [f"{srv} M {CHAN} +o {n['bob']} {ts}"]
    assert await sent(f"clearchan {CHAN} b") == [f"{srv} CM {CHAN} :b"]
    assert await sent(f"clearchan {CHAN} ALL") == [f"{srv} CM {CHAN} :obklimnsptrDCcuMZ"]


@pytest.mark.asyncio
async def test_say_and_do(ccontrol_linked):
    hub, _proc = ccontrol_linked
    oper, n, _ts = await _setup(hub)
    srv, bot = hub.peer_numeric, cc.numnick(hub)

    async def sent(command: str) -> list[str]:
        return cc.network(await cc.run(hub, oper, command), oper)

    assert await sent(f"say -s {CHAN} from the server") == [f"{srv} P {CHAN} :from the server"]
    assert await sent(f"say -b {CHAN} from the bot") == [f"{bot} P {CHAN} :from the bot"]
    assert await sent(f"do -b {CHAN} waves") == [f"{bot} P {CHAN} :\x01ACTION waves\x01"]
    assert await sent("say -s alice to one client") == [f"{srv} P {n['alice']} :to one client"]
    assert await sent("say -b alice to one client") == [f"{bot} P {n['alice']} :to one client"]


@pytest.mark.asyncio
async def test_announce_is_a_notice_to_everybody_from_a_client_made_for_it(ccontrol_linked):
    hub, _proc = ccontrol_linked
    oper, _n, _ts = await _setup(hub)

    sent = cc.network(await cc.run(hub, oper, "announce Maintenance in ten minutes"), oper)
    assert len(sent) == 3, sent
    introduced, notice, quit_line = sent
    announcer = introduced.split(" :", 1)[0].split(" ")[-1]
    assert p10_token(introduced) == "N" and " announce " in introduced
    assert notice == f"{announcer} O $* :Maintenance in ten minutes"
    assert quit_line.startswith(f"{announcer} Q :")


@pytest.mark.asyncio
async def test_topic_is_set_by_the_server_in_the_form_the_link_takes(ccontrol_linked):
    """The handwritten line was "T #chan :text", which a P11 uplink does not
    take. xServer::Topic() knows the link, and keeps our own record of the topic."""
    hub, _proc = ccontrol_linked
    oper, _n, _ts = await _setup(hub)

    sent = cc.network(await cc.run(hub, oper, f"topic {CHAN} hello there"), oper)
    assert sent == [f"{hub.peer_numeric} T {CHAN} :(operone) hello there"]
