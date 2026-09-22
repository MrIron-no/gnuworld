"""Explore oversized nick/user/host/realname/account from a fake service leaf.

Ircu-style limits under test (exceeded by +1 unless noted):

  USERLEN         10
  HOSTLEN         63
  ACCOUNTLEN      12
  REALLEN         50
  max_nick_length 15

FakeHub pretends to be the ircu hub; a downstream leaf acts as a service
server and introduces clients with fields past those limits. mod.debug
USERINFO reports what gnuworld stored.
"""

from __future__ import annotations

import asyncio
import time

import pytest

from p10 import client_numnick, ipv4_to_b64, p10_token

PERMIT_ACCOUNT = "MrIron"
LEAF_NAME = "services-leaf.testnet"
LEAF_NUMERIC = 3

# Exact ircu limits, then +1 for the "too long" variants.
USERLEN = 10
HOSTLEN = 63
ACCOUNTLEN = 12
REALLEN = 50
MAX_NICK = 15


def _pad(prefix: str, length: int) -> str:
    """Build an ASCII string of exactly ``length`` chars starting with prefix."""
    if length < len(prefix):
        return prefix[:length]
    return prefix + ("x" * (length - len(prefix)))


OVERSIZE = {
    "nick": _pad("LongNick", MAX_NICK + 1),  # 16
    "user": _pad("longident", USERLEN + 1),  # 11
    "host": _pad("longhost.", HOSTLEN + 1),  # 64
    "real": _pad("Long realname field that goes over the limit-", REALLEN + 1),  # 51
    "account": _pad("LongAccount", ACCOUNTLEN + 1),  # 13
}


async def _authed_querier(hub) -> str:
    """Introduce a hub nick allowed to talk to stealth debug."""
    numnick = await hub.introduce_nick("querier", username="querier")
    await hub.send_account(numnick, PERMIT_ACCOUNT)
    return numnick


async def _introduce_leaf_client(
    hub,
    leaf_yy: str,
    leaf_numeric: int,
    *,
    nick: str,
    client_num: int,
    username: str,
    host: str,
    realname: str,
    account: str | None = None,
) -> str:
    """Introduce a leaf nick; optionally set account via ``+r`` on the N line."""
    ts = int(time.time())
    numnick = client_numnick(leaf_numeric, client_num)
    ip64 = ipv4_to_b64("127.0.0.1")
    if account is not None:
        # N ... +ir <account> <ip> <numnick> :<realname>
        await hub.send_raw(
            f"{leaf_yy} N {nick} 1 {ts} {username} {host} +ir {account} "
            f"{ip64} {numnick} :{realname}"
        )
        hub.users[nick.lower()] = {
            "nick": nick,
            "numnick": numnick,
            "user": username,
            "host": host,
            "modes": "+ir",
            "realname": realname,
            "account": account,
            "ip": "127.0.0.1",
            "server": leaf_yy,
        }
    else:
        numnick = await hub.introduce_leaf_nick(
            leaf_yy,
            leaf_numeric,
            nick,
            client_num=client_num,
            username=username,
            host=host,
            realname=realname,
        )
    return numnick


async def _userinfo_notices(hub, querier: str, target: str, timeout: float = 10.0) -> list[str]:
    """PRIVMSG debug USERINFO and collect one complete Notice reply set.

    Waits until a terminal notice (``On channels:`` / not-found / service agent)
    so replies from consecutive queries cannot interleave.
    """
    await hub.drain_messages(timeout=0.3)
    after = len(hub.received)
    await hub.send_privmsg(querier, f"debug@{hub.peer_name}", f"USERINFO {target}")

    notices: list[str] = []
    deadline = asyncio.get_event_loop().time() + timeout
    cursor = after
    while True:
        remaining = deadline - asyncio.get_event_loop().time()
        if remaining <= 0:
            break

        def is_notice(line: str) -> bool:
            return p10_token(line) == "O" and querier in line

        try:
            line = await hub.wait_for(is_notice, timeout=min(remaining, 2.0), after=cursor)
        except TimeoutError:
            if notices:
                continue
            break

        # Advance past this exact occurrence (index() would rewind on duplicate text).
        try:
            pos = hub.received.index(line, cursor)
        except ValueError:
            pos = len(hub.received) - 1
        cursor = pos + 1

        text = line.split(":", 1)[-1] if ":" in line else line
        notices.append(text)
        if (
            "On channels:" in text
            or "is a service agent" in text
            or "Unable to find" in text
            or "No such" in text
        ):
            break
    return notices


@pytest.mark.asyncio
async def test_oversized_fields_from_service_leaf(debug_linked):
    """Service leaf introduces clients past ircu length limits; observe gnuworld."""
    hub, proc = debug_linked

    leaf_yy = await hub.introduce_leaf(
        LEAF_NAME,
        LEAF_NUMERIC,
        flags="",
        description="Fake service server",
    )
    await hub.end_burst(leaf_yy)

    querier = await _authed_querier(hub)

    cases = [
        {
            "label": "oversize_nick",
            "nick": OVERSIZE["nick"],
            "username": "okuser",
            "host": "ok.host.testnet",
            "realname": "Ok Realname",
            "account": None,
        },
        {
            "label": "oversize_user",
            "nick": "OkNickUser",
            "username": OVERSIZE["user"],
            "host": "ok.host.testnet",
            "realname": "Ok Realname",
            "account": None,
        },
        {
            "label": "oversize_host",
            "nick": "OkNickHost",
            "username": "okuser",
            "host": OVERSIZE["host"],
            "realname": "Ok Realname",
            "account": None,
        },
        {
            "label": "oversize_real",
            "nick": "OkNickReal",
            "username": "okuser",
            "host": "ok.host.testnet",
            "realname": OVERSIZE["real"],
            "account": None,
        },
        {
            "label": "oversize_account",
            "nick": "OkNickAcct",
            "username": "okuser",
            "host": "ok.host.testnet",
            "realname": "Ok Realname",
            "account": OVERSIZE["account"],
        },
        {
            "label": "all_oversize",
            "nick": OVERSIZE["nick"] + "Y",  # 17 — distinct from oversize_nick
            "username": OVERSIZE["user"],
            "host": OVERSIZE["host"],
            "realname": OVERSIZE["real"],
            "account": OVERSIZE["account"],
        },
    ]

    results: list[dict] = []

    for i, case in enumerate(cases):
        # Sanity: confirm we exceeded each limit we claim to.
        if case["label"] == "oversize_nick" or case["label"] == "all_oversize":
            assert len(case["nick"]) > MAX_NICK
        if case["label"] in ("oversize_user", "all_oversize"):
            assert len(case["username"]) > USERLEN
        if case["label"] in ("oversize_host", "all_oversize"):
            assert len(case["host"]) > HOSTLEN
        if case["label"] in ("oversize_real", "all_oversize"):
            assert len(case["realname"]) > REALLEN
        if case["account"] is not None:
            assert len(case["account"]) > ACCOUNTLEN

        numnick = await _introduce_leaf_client(
            hub,
            leaf_yy,
            LEAF_NUMERIC,
            nick=case["nick"],
            client_num=10 + i,
            username=case["username"],
            host=case["host"],
            realname=case["realname"],
            account=case["account"],
        )

        await hub.drain_messages(timeout=0.4)

        # Did gnuworld SQ us / ERROR / die?
        errors = [
            line
            for line in hub.received
            if p10_token(line) in {"ERROR", "SQ", "Y"}  # Y = ERROR in some maps
            or " ERROR " in f" {line} "
        ]
        alive = proc.proc is not None and proc.proc.returncode is None

        notices = await _userinfo_notices(hub, querier, case["nick"])
        if not notices:
            # Try by numeric if nick lookup failed (e.g. truncated nick).
            notices = await _userinfo_notices(hub, querier, f"-num {numnick}")

        results.append(
            {
                "label": case["label"],
                "sent": {
                    "nick": case["nick"],
                    "nick_len": len(case["nick"]),
                    "user": case["username"],
                    "user_len": len(case["username"]),
                    "host": case["host"],
                    "host_len": len(case["host"]),
                    "real": case["realname"],
                    "real_len": len(case["realname"]),
                    "account": case["account"],
                    "account_len": len(case["account"]) if case["account"] else 0,
                    "numnick": numnick,
                },
                "alive": alive,
                "errors": errors[-5:],
                "notices": notices,
            }
        )

        assert alive, f"gnuworld died after introducing {case['label']}"

    # Pretty dump for ``pytest -s``
    print("\n=== Oversized field experiment (service leaf → gnuworld) ===")
    print(
        f"Limits: nick={MAX_NICK} user={USERLEN} host={HOSTLEN} "
        f"real={REALLEN} account={ACCOUNTLEN}"
    )
    for r in results:
        s = r["sent"]
        print(f"\n-- {r['label']} (alive={r['alive']}) --")
        print(
            f"  sent nick={s['nick']!r}({s['nick_len']}) "
            f"user={s['user']!r}({s['user_len']}) "
            f"host={s['host']!r}({s['host_len']}) "
            f"real={s['real']!r}({s['real_len']}) "
            f"account={s['account']!r}({s['account_len']})"
        )
        if r["errors"]:
            print(f"  hub errors/SQ: {r['errors']}")
        if not r["notices"]:
            print("  USERINFO: (no notices — client not found?)")
        for n in r["notices"]:
            print(f"  USERINFO: {n}")

    # Soft expectations: gnuworld uses std::string and does not enforce
    # ircu USERLEN/HOSTLEN/etc., so USERINFO should still find the clients
    # under the nick we sent.
    for r in results:
        assert r["notices"], f"USERINFO returned nothing for {r['label']}"
        joined = "\n".join(r["notices"])
        assert "Unable to find" not in joined and "No such" not in joined, (
            f"gnuworld did not retain client for {r['label']}: {joined}"
        )

    # Spot-check that full oversized values survived into gnuworld state.
    by_label = {r["label"]: r for r in results}

    nick_notices = "\n".join(by_label["oversize_nick"]["notices"])
    assert OVERSIZE["nick"] in nick_notices, nick_notices

    user_notices = "\n".join(by_label["oversize_user"]["notices"])
    assert OVERSIZE["user"] in user_notices, user_notices

    host_notices = "\n".join(by_label["oversize_host"]["notices"])
    assert OVERSIZE["host"] in host_notices, host_notices

    real_notices = "\n".join(by_label["oversize_real"]["notices"])
    assert OVERSIZE["real"] in real_notices, real_notices

    acct_notices = "\n".join(by_label["oversize_account"]["notices"])
    assert OVERSIZE["account"] in acct_notices, acct_notices
