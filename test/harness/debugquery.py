"""Read gnuworld's internal state back through mod.debug.

Tests use this to assert on what gnuworld believes after it has processed
some server-to-server traffic, rather than on the traffic itself.
"""

from __future__ import annotations

import calendar
import re
import time
from dataclasses import dataclass, field

from p10 import p10_token

# "  *!*@bad.host  set by SomeNick  at 2026-09-19 19:09:14 (0 days, 00:03:00 ago)",
# or "... at (unknown)" for a ban whose details the core does not have.
BAN_LINE = re.compile(r"^  (?P<mask>\S+)  set by (?P<setby>.+?)  at (?P<at>.+)$")


def _ban_time(at: str) -> int | None:
    """The epoch seconds of a ban line's "at" field, or None if it has none.

    mod.debug prints the absolute part with misc.h's prettyTime(), which is
    UTC ("%F %H:%M:%S"), followed by prettyDuration() in brackets.
    """
    m = re.match(r"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})", at)
    if not m:
        return None
    return calendar.timegm(time.strptime(m.group(1), "%Y-%m-%d %H:%M:%S"))


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
    # One entry per ban: the nick or server name CHANINFO names, "(unknown)"
    # where it has none, and when the ban was set (UTC epoch seconds).
    ban_setters: dict[str, str] = field(default_factory=dict)
    ban_times: dict[str, int] = field(default_factory=dict)


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
        elif state["bans_expected"] >= 0 and (m := BAN_LINE.match(text)):
            # One notice per ban: the mask, who set it and when
            mask = m.group("mask")
            info.bans.add(mask)
            info.ban_setters[mask] = m.group("setby")
            if (when := _ban_time(m.group("at"))) is not None:
                info.ban_times[mask] = when
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
