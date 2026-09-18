"""Drive mod.ccontrol (euworld) as a logged in oper.

``run`` sends one command and returns the lines gnuworld sent to the network
because of it. ccontrol answers an iauth CHECK on the same connection and in
order, so the XR for a CHECK sent right behind the command marks the end.
"""

from __future__ import annotations

import itertools

from p10 import p10_token, strip_msg_tags

OPER_USER = "Admin"
OPER_PASSWORD = "testpass"  # docker/initdb/03_harness_oper.sql

_routing = itertools.count(1)


def numnick(hub) -> str:
    found = hub.get_user_numnick("euworld")
    assert found, "gnuworld did not introduce euworld"
    return found


async def run(hub, oper: str, command: str, timeout: float = 10.0) -> list[str]:
    routing = f"iauth:sync{next(_routing)}"
    after = len(hub.received)
    await hub.send_privmsg(oper, numnick(hub), command)
    await hub.send_xquery(routing=routing, message="CHECK sync sync 192.0.2.250 sync.testnet :sync")

    def is_sentinel(line: str) -> bool:
        return p10_token(line) == "XR" and routing in line

    await hub.wait_for(is_sentinel, timeout=timeout, after=after)

    lines = []
    for line in hub.received[after:]:
        if is_sentinel(line):
            break
        lines.append(strip_msg_tags(line))
    return lines


def network(lines: list[str], oper: str, log_channels: tuple[str, ...] = ("#gnuworld.message",)) -> list[str]:
    """What is left when the replies to the oper and the messages to ccontrol's
    log channels are taken out."""
    kept = []
    for line in lines:
        fields = line.split(" :", 1)[0].split(" ")
        if p10_token(line) in ("O", "P") and len(fields) > 2:
            if fields[2] == oper or fields[2].lower() in log_channels:
                continue
        kept.append(line)
    return kept


async def login(hub, nick: str = "operone") -> str:
    oper = await hub.introduce_nick(nick, username=nick, modes="+io")
    replies = await run(hub, oper, f"login {OPER_USER} {OPER_PASSWORD}")
    assert any("Authentication successful" in line or "uthenticat" in line for line in replies), replies
    return oper
