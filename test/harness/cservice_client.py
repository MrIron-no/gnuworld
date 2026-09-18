"""Drive mod.cservice (X) as a logged in user.

``run`` sends one command and returns the lines gnuworld sent to the network
because of it. X handles messages in order, so its answer to a second command
that names a nick nobody has marks the end.
"""

from __future__ import annotations

import asyncio
import itertools

from p10 import p10_token, strip_msg_tags

ADMIN_USER = "Admin"
ADMIN_PASSWORD = "temPass2020@"  # doc/cservice.addme.sql

_sync = itertools.count(1)


def numnick(hub) -> str:
    found = hub.get_user_numnick("X")
    assert found, "gnuworld did not introduce X"
    return found


async def run(hub, user: str, command: str, timeout: float = 10.0) -> list[str]:
    marker = f"sync{next(_sync)}"
    numnick(hub)
    # "X@<server>": LOGIN is refused any other way, and every command takes it
    target = f"X@{hub.peer_name}"
    after = len(hub.received)
    await hub.send_privmsg(user, target, command)
    await hub.send_privmsg(user, target, f"verify {marker}")

    def is_sentinel(line: str) -> bool:
        return p10_token(line) == "O" and f" {user} :" in line and marker in line

    await hub.wait_for(is_sentinel, timeout=timeout, after=after)

    lines = []
    for line in hub.received[after:]:
        if is_sentinel(line):
            break
        lines.append(strip_msg_tags(line))
    return lines


def network(lines: list[str], user: str, relay: str = "#coder-com") -> list[str]:
    """Without X's replies to the user and its messages to its log channel."""
    kept = []
    for line in lines:
        fields = line.split(" :", 1)[0].split(" ")
        if p10_token(line) in ("O", "P") and len(fields) > 2:
            if fields[2] == user or fields[2].lower() == relay:
                continue
        kept.append(line)
    return kept


async def login(hub, nick: str = "adminone") -> str:
    user = await hub.introduce_nick(nick, username=nick)
    # X takes no login in the second it connected in, login_delay or none
    for _attempt in range(5):
        replies = await run(hub, user, f"login {ADMIN_USER} {ADMIN_PASSWORD}")
        if any("AUTHENTICATION SUCCESSFUL" in line.upper() for line in replies):
            return user
        assert any("during reconnection" in line for line in replies), replies
        await asyncio.sleep(0.5)
    raise AssertionError(replies)
