"""Read gnuworld's internal state back through mod.debug.

Tests use this to assert on what gnuworld believes after it has processed
some server-to-server traffic, rather than on the traffic itself.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

from p10 import p10_token


@dataclass
class ChanInfo:
    """What mod.debug CHANINFO reports about one channel."""

    found: bool = False
    modes: str = ""  # mode letters only, sorted: "Dnt"
    created: int = 0
    key: str | None = None
    limit: int | None = None
    topic: str = ""
    members: dict[str, str] = field(default_factory=dict)  # nick -> none | hide | +v | +o | +o+v
    bans: set[str] = field(default_factory=set)


async def chaninfo(hub, asker: str, channel: str, timeout: float = 10.0) -> ChanInfo:
    """PRIVMSG ``CHANINFO <channel>`` to mod.debug as ``asker`` and parse the reply.

    ``asker`` must be a numnick logged in to the account mod.debug permits.
    """
    info = ChanInfo()
    state = {"bans_expected": -1}

    def notice_text(line: str) -> str | None:
        if p10_token(line) != "O" or f" {asker} :" not in line:
            return None
        return line.split(f" {asker} :", 1)[1]

    def feed(text: str) -> bool:
        """Consume one notice. Returns True once the reply is complete."""
        if text.startswith("Unable to find channel"):
            return True
        info.found = True
        if m := re.match(r"Modes: \+(\S*)", text):
            info.modes = "".join(sorted(m.group(1)))
        elif m := re.match(r"Created at time: (\d+)", text):
            info.created = int(m.group(1))
        elif text.startswith("Key: "):
            info.key = text[5:]
        elif text.startswith("Limit: "):
            info.limit = int(text[7:])
        elif text.startswith("Topic: "):
            info.topic = text[7:]
        elif "(numeric: " in text:
            # "  +o:   nick!user@host (numeric: ABAAC)"; the label is padded
            status, rest = text.strip().split(":", 1)
            info.members[rest.strip().split("!", 1)[0]] = status.strip()
        elif text.startswith("Ban list: (empty)"):
            return True
        elif m := re.match(r"Ban list \((\d+)\):", text):
            state["bans_expected"] = int(m.group(1))
        elif state["bans_expected"] >= 0 and text.startswith("  "):
            info.bans.update(mask.strip() for mask in text.split(","))
            return len(info.bans) >= state["bans_expected"]
        return False

    after = len(hub.received)
    await hub.send_privmsg(asker, f"debug@{hub.peer_name}", f"CHANINFO {channel}")

    seen = after

    def complete(_line: str) -> bool:
        nonlocal seen
        done = False
        while seen < len(hub.received):
            text = notice_text(hub.received[seen])
            seen += 1
            if text is not None and feed(text):
                done = True
        return done

    await hub.wait_for(complete, timeout=timeout, after=after)
    return info
