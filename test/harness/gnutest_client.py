"""Drive mod.gnutest, which calls gnuworld's core API from chat commands.

``run`` sends one command and returns exactly the lines gnuworld sent to the
network because of it, which is how outbound behaviour is tested.
"""

from __future__ import annotations

from p10 import p10_token, strip_msg_tags

SENTINEL_REPLY = "Are you speaking to me?"


def numnick(hub) -> str:
    """gnutest's numnick, learned from the N line gnuworld burst for it."""
    found = hub.get_user_numnick("gnutest")
    assert found, "gnuworld did not introduce a gnutest client"
    return found


async def run(hub, asker: str, command: str, timeout: float = 10.0) -> list[str]:
    """Send ``command`` to gnutest as ``asker``; return what gnuworld sent for it.

    gnuworld handles inbound lines in order and writes its output in order, so
    a second, meaningless message marks the end: gnutest answers a one-word
    message with a fixed notice. Everything received before that notice belongs
    to ``command``. Message tags are stripped from the returned lines.
    """
    target = numnick(hub)
    after = len(hub.received)
    await hub.send_privmsg(asker, target, command)
    await hub.send_privmsg(asker, target, "sync")

    def is_sentinel(line: str) -> bool:
        return p10_token(line) == "O" and f" {asker} :" in line and SENTINEL_REPLY in line

    await hub.wait_for(is_sentinel, timeout=timeout, after=after)

    lines = []
    for line in hub.received[after:]:
        if is_sentinel(line):
            break
        lines.append(strip_msg_tags(line))
    return lines
