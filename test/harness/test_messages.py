"""PRIVMSG, NOTICE and WALLCHOPS: xServer::SendMessage() and friends.

Every message gnuworld sends is put together in one place. A line break in the
text starts another message, and a line too long for one message is continued
in the next, where the uplink would otherwise truncate it.
"""

from __future__ import annotations

import time

import pytest

import gnutest_client as gt
from p10 import p10_token, strip_msg_tags

CHAN = "#talk"


async def _setup(hub) -> dict:
    asker = await hub.introduce_nick("asker", username="asker")
    alice = await hub.introduce_nick("alice", username="alice")
    ts = int(time.time()) - 3600
    await hub.send_raw(f"{hub.server_numnick} B {CHAN} {ts} +n {alice}:o")
    await gt.run(hub, asker, f"join {CHAN}")
    return dict(asker=asker, alice=alice, ts=ts, me=gt.numnick(hub))


@pytest.mark.asyncio
async def test_a_long_message_is_continued_not_truncated(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    v = await _setup(hub)

    words = " ".join(f"word{i:03d}" for i in range(120))  # 959 characters
    sent = await gt.run(hub, v["asker"], f"say {CHAN} {words}")

    prefix = f"{v['me']} P {CHAN} :"
    assert len(sent) == 2
    assert all(line.startswith(prefix) and len(line) <= 510 for line in sent)
    # Broken between words, and nothing lost
    assert " ".join(line[len(prefix):] for line in sent) == words


@pytest.mark.asyncio
async def test_each_line_of_a_text_is_a_message(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    v = await _setup(hub)

    sent = await gt.run(hub, v["asker"], f"say {CHAN} first\rsecond\r\rthird")
    assert sent == [f"{v['me']} P {CHAN} :{text}" for text in ("first", "second", "third")]


@pytest.mark.asyncio
async def test_messages_from_a_fake_client(gnutest_linked_p11):
    hub, _proc = gnutest_linked_p11
    v = await _setup(hub)

    spawned = await gt.run(hub, v["asker"], "spawnclient fakey")
    fake = next(strip_msg_tags(l).split(" :", 1)[0].split(" ")[-1]
                for l in spawned if p10_token(l) == "N")

    async def run(command: str) -> list[str]:
        return await gt.run(hub, v["asker"], command)

    assert await run(f"fakesay fakey {CHAN} hello") == [f"{fake} P {CHAN} :hello"]
    assert await run("fakesay fakey alice psst") == [f"{fake} P {v['alice']} :psst"]
    assert await run("fakenotice fakey alice fyi") == [f"{fake} O {v['alice']} :fyi"]
    # FakeNotice() to a channel sent a PRIVMSG, having been copied from FakeMessage()
    assert await run(f"fakenotice fakey {CHAN} fyi all") == [f"{fake} O {CHAN} :fyi all"]
