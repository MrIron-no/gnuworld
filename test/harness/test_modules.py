"""mod.nickserv, mod.dronescan and mod.openchanfix on a P11 link.

Each wrote some of its lines itself until they were moved to the core API: the
console topics, the WHOIS numerics, a copy of the loop that cuts a text into
messages. These are the first harness tests of the three; they cover what was
changed, not what the modules are for.
"""

from __future__ import annotations

import time

import pytest

from debugquery import chaninfo
from p10 import p10_token, strip_msg_tags

DEBUG_ACCOUNT = "MrIron"


async def _lines_until(hub, after: int, last: str, timeout: float = 10.0) -> list[str]:
    await hub.wait_for(lambda l: last in strip_msg_tags(l), timeout=timeout, after=after)
    return [strip_msg_tags(l) for l in hub.received[after:]]


def _topic_of(lines: list[str], channel: str) -> list[str]:
    (line,) = [l for l in lines if p10_token(l) == "T" and f" {channel} " in l]
    fields, text = line.split(" :", 1)
    return fields.split(" ") + [text]


@pytest.mark.asyncio
async def test_nickserv_sets_its_console_topic_in_the_form_of_the_link(nickserv_linked):
    hub, _proc = nickserv_linked
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, DEBUG_ACCOUNT)
    ns = hub.get_user_numnick("NS")

    source, _t, channel, chan_ts, topic_ts, text = _topic_of([strip_msg_tags(l) for l in hub.received],
                                                             "#ns.console")
    assert source == ns and chan_ts.isdigit() and abs(int(topic_ts) - time.time()) < 300
    assert text.startswith("Current NickServ console level:")
    info = await chaninfo(hub, asker, "#ns.console")
    assert info.topic == text and info.created == int(chan_ts)


@pytest.mark.asyncio
async def test_dronescan_links_next_to_a_stealth_module_and_sets_its_topic(dronescan_linked):
    """Its burst gave every xClient on the server a record of its own, through
    getInstance(), which a stealth module does not have: gnuworld crashed while
    linking whenever one was loaded, as mod.debug is here."""
    hub, _proc = dronescan_linked
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, DEBUG_ACCOUNT)

    fields = _topic_of([strip_msg_tags(l) for l in hub.received], "#ds.console")
    assert fields[0] == hub.get_user_numnick("E") and "channelMargin" in fields[-1]
    assert (await chaninfo(hub, asker, "#ds.console")).topic == fields[-1]


@pytest.mark.asyncio
async def test_dronescan_answers_a_whois_with_numerics_from_the_server(dronescan_linked):
    hub, _proc = dronescan_linked
    alice = await hub.introduce_nick("alice", username="alice")
    srv = hub.peer_numeric

    after = len(hub.received)
    await hub.send_raw(f"{alice} W {srv} :E")
    lines = [l for l in await _lines_until(hub, after, " 318 ") if l.startswith(f"{srv} 3")]
    assert lines[0] == f"{srv} 311 {alice} E dronescan undernet.org * :Drone Scanner"
    assert lines[-1] == f"{srv} 318 {alice} E :End of /WHOIS list."
    assert all(l.split(" ")[2] == alice for l in lines)


@pytest.mark.asyncio
async def test_openchanfix_sends_every_line_of_a_text_as_a_notice(openchanfix_linked):
    """SendTo() and SendFmtTo(): the second had its own loop that cut a text at
    its line breaks into 509 byte pieces. Notice() does that."""
    hub, _proc = openchanfix_linked
    oper = await hub.introduce_nick("operone", username="operone", modes="+io")
    c = hub.get_user_numnick("C")

    after = len(hub.received)
    await hub.send_privmsg(oper, c, "help")
    await hub.send_privmsg(oper, c, "help score")
    lines = await _lines_until(hub, after, "SCORE <#channel>")
    replies = [l for l in lines if l.startswith(f"{c} O {oper} :")]
    # chanfix writes its headings in bold
    assert any("Oper Level:" in l.replace("\x02", "") for l in replies), replies
    assert all("\n" not in l and "\r" not in l and len(l) <= 510 for l in replies)
