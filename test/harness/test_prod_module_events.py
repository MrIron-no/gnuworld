"""What the production modules do when the network tells them something.

mod.cservice, mod.ccontrol, mod.dronescan and mod.openchanfix each receive
every network event through one ``OnEvent(const eventType&, void*, void*,
void*, void*)`` and cast the four ``void*`` back by hand.  That is about to
become one named virtual per event with typed parameters, and the handlers
will be rewritten mechanically.  Nothing tests them today: the harness's other
module tests drive COMMANDS.

So this file pins the EVENTS, from the outside, before the rewrite: for each
event a module registers for, the cheapest consequence a fake hub can see -
a line on the wire, a counter a command reports, a state a command names, a
line gnuworld writes to its console.  Every test also asserts gnuworld is
still running afterwards, because several of these paths abort (an assert, a
null dereference) rather than misbehave, and a dead daemon must not read as a
pass.

Events with no cheap observable consequence are NOT invented here; they are
listed in the report that comes with this file, per module and per event, for
the reviewers of the rewrite to check by eye.
"""

from __future__ import annotations

import asyncio
import itertools
import os
import re
import subprocess
import time
from contextlib import asynccontextmanager

import pytest

import ccontrol_client as cc
import cservice_client as cs
from conftest import (
    _prepare_conf_dir,
    link_ccontrol_logging,
    link_cservice_logging,
    link_module,
    require_module,
)
from gnuworld_proc import COMPOSE_FILE, CONTAINER_UPLINK, HARNESS_DIR, GnuworldProc
from p10 import p10_token, strip_msg_tags

PERMIT_ACCOUNT = "MrIron"  # the account mod.debug's harness config permits

_serial = itertools.count(1)


def unique(prefix: str) -> str:
    """A name no parallel run and no earlier run in this database shares."""
    return f"{prefix}{os.getpid()}x{next(_serial)}"


def psql(sql: str, db: str) -> None:
    """One statement against the harness's Postgres, as test_logging.py does."""
    subprocess.run(
        ["docker", "compose", "-f", str(COMPOSE_FILE), "exec", "-T", "postgres",
         "psql", "-U", "gnuworld", "-d", db, "-c", sql],
        cwd=str(HARNESS_DIR),
        check=True,
        capture_output=True,
    )


def said(proc, needle: str) -> int:
    """How many times gnuworld has written ``needle`` to its console.

    A count, because wait_for_stdout() always searches from the first line: a
    module that says the same thing again needs the number, not the line.
    """
    return sum(1 for line in proc.stdout_lines if needle in line)


async def wait_said(proc, needle: str, times: int, timeout: float = 25.0) -> None:
    deadline = time.monotonic() + timeout
    while said(proc, needle) < times:
        assert proc.proc is None or proc.proc.returncode is None, "gnuworld exited"
        assert time.monotonic() < deadline, (
            f"{needle!r} was said {said(proc, needle)} times, not {times}:\n"
            + "\n".join(proc.stdout_lines[-30:])
        )
        await asyncio.sleep(0.2)


def alive(proc) -> None:
    """These modules abort on some event paths; a silent abort is not a pass."""
    assert proc.proc is not None and proc.proc.returncode is None, (
        "gnuworld is no longer running:\n" + "\n".join(proc.stdout_lines[-30:])
    )


# --------------------------------------------------------------------------
# Talking to a module that answers one client at a time
# --------------------------------------------------------------------------


def _reply_to(bot: str, who: str, line: str) -> str | None:
    """The text of ``line`` if it is a notice or a message from bot to who."""
    payload = strip_msg_tags(line)
    head, _, text = payload.partition(" :")
    fields = head.split(" ")
    if len(fields) != 3 or fields[0] != bot or fields[1] not in ("O", "P") or fields[2] != who:
        return None
    return text


async def ask(hub, who: str, bot: str, command: str, last: str | tuple[str, ...],
              timeout: float = 20.0) -> list[str]:
    """Send ``command`` to ``bot`` as ``who``; return its replies.

    ``last`` is a piece of the final reply the command produces, or several
    pieces of which any one ends it - these modules answer one client's command
    completely before they read the next line, so it marks the end.
    """
    endings = (last,) if isinstance(last, str) else last
    after = len(hub.received)
    await hub.send_privmsg(who, bot, command)

    def done(line: str) -> bool:
        text = _reply_to(bot, who, line)
        return text is not None and any(ending in text for ending in endings)

    await hub.wait_for(done, timeout=timeout, after=after)
    replies = []
    for line in hub.received[after:]:
        text = _reply_to(bot, who, line)
        if text is not None:
            replies.append(text)
    return replies


def burst_ts(hub, channel: str) -> int:
    """The creation time gnuworld gave a channel it bursted itself.

    A module's own channel is created by its burst; a B line from the hub with
    an older time is a takeover, which clears the channel's modes and deops the
    module, so a test that wants the module to act has to use its time.
    """
    found = None
    for line in hub.received:
        fields = strip_msg_tags(line).split(" ")
        if len(fields) > 3 and fields[1] == "B" and fields[2].lower() == channel.lower():
            found = int(fields[3])
    assert found is not None, f"gnuworld did not burst {channel}"
    return found


async def console(hub, bot: str, channel: str, after: int, needle: str,
                  timeout: float = 25.0) -> str:
    """Wait for a line ``bot`` writes to its console channel containing ``needle``."""
    def matches(line: str) -> bool:
        payload = strip_msg_tags(line)
        head, _, text = payload.partition(" :")
        fields = head.split(" ")
        return (len(fields) == 3 and fields[0] == bot and fields[1] in ("O", "P")
                and fields[2].lower() == channel.lower() and needle in text)

    return strip_msg_tags(await hub.wait_for(matches, timeout=timeout, after=after))


# ==========================================================================
# mod.cservice
# ==========================================================================
#
# EVT_NICK, EVT_ACCOUNT, EVT_QUIT and EVT_KILL all maintain one thing X can be
# asked about: sqlUser::networkClientList, the clients authenticated as an
# account.  STATUS names them ("Auth: Admin/<nicks>[500]"), so one command
# reads back what four handlers wrote.


AUTH_LINE = re.compile(r"^Auth: Admin/(?P<nicks>[^\[]*)\[")


async def authed_nicks(hub, user: str) -> set[str]:
    """The nicks X reports as authenticated to "Admin", through STATUS.

    #coder-com is registered by doc/cservice.addme.sql and Admin has 500 on
    it, so this needs no channel of its own.
    """
    replies = [line for line in await cs.run(hub, user, "status #coder-com")
               if p10_token(line) == "O" and f" {user} :" in line]
    texts = [line.split(f" {user} :", 1)[1].replace("\x02", "") for line in replies]
    matched = [AUTH_LINE.match(text) for text in texts]
    found = [m for m in matched if m]
    assert found, f"no Auth line in X's STATUS reply: {texts}"
    return set(found[-1].group("nicks").split())


@pytest.mark.asyncio
async def test_cservice_tracks_who_is_authenticated_through_four_events(cservice_linked):
    """EVT_NICK (+r at introduction), EVT_ACCOUNT (an AC afterwards), EVT_QUIT
    and EVT_KILL, all seen through the one list X keeps of an account's
    clients."""
    hub, proc = cservice_linked
    admin = await cs.login(hub)
    assert await authed_nicks(hub, admin) == {"adminone"}

    # EVT_NICK: a client that arrives already logged in (umode +r <account>)
    await hub.introduce_nick("bursted", username="bursted", modes="+ir Admin")
    assert await authed_nicks(hub, admin) == {"adminone", "bursted"}

    # EVT_ACCOUNT: a client that logs in after it connected
    late = await hub.introduce_nick("lateone", username="lateone")
    assert await authed_nicks(hub, admin) == {"adminone", "bursted"}
    await hub.send_account(late, "Admin")
    assert await authed_nicks(hub, admin) == {"adminone", "bursted", "lateone"}

    # EVT_QUIT
    await hub.send_raw(f"{late} Q :gone")
    assert await authed_nicks(hub, admin) == {"adminone", "bursted"}

    # EVT_KILL
    victim = hub.get_user_numnick("bursted")
    await hub.send_raw(f"{hub.server_numnick} D {victim} hub.testnet!oper :(bye)")
    assert await authed_nicks(hub, admin) == {"adminone"}
    alive(proc)


@pytest.mark.asyncio
async def test_cservice_answers_an_xquery_and_forgets_a_split_servers_requests(cservice_linked):
    """EVT_XQUERY: ISUSER, which X answers with an XR of its own.

    EVT_NETBREAK only drops SASL requests still waiting for that server, so it
    is exercised here (the link's own leaf is squitted) but only asserted on
    negatively: X carries on.
    """
    hub, proc = cservice_linked

    async def isuser(name: str) -> list[str]:
        routing = unique("isuser:")
        after = len(hub.received)
        await hub.send_xquery(routing=routing, message=f"ISUSER +{name}")
        await hub.wait_for(
            lambda line: p10_token(line) == "XR" and routing in line
            and line.split(" :", 1)[1] == "ISUSER EOL", timeout=15.0, after=after)
        return [strip_msg_tags(line).split(" :", 1)[1] for line in hub.received[after:]
                if p10_token(line) == "XR" and routing in line]

    assert [reply for reply in await isuser("Admin") if reply.startswith("ISUSER YES Admin:1:")]
    assert await isuser("NoSuchUser") == ["ISUSER NO +NoSuchUser ", "ISUSER EOL"]

    leaf = await hub.introduce_leaf("split.testnet", 42, protocol="J11")
    await hub.end_burst(leaf)
    await hub.send_raw(f"{hub.server_numnick} SQ split.testnet 0 :bye")
    assert [reply for reply in await isuser("Admin") if reply.startswith("ISUSER YES Admin:1:")]
    alive(proc)


async def _register(hub, admin: str, chan: str, members: str, ts: int) -> None:
    """REGISTER ``chan`` to Admin, and clean up after itself if that goes wrong.

    The assert below is exactly what a regression in X's REGISTER reply would
    trip, and by then REGISTER has already written its rows: they have to go
    whatever happens, or a failure here leaves them in a database that is shared
    with every other test and with any parallel run.  The caller's own
    ``finally`` cannot do it, because there is nothing to enter a ``try`` for
    until this returns.
    """
    await hub.send_raw(f"{hub.server_numnick} B {chan} {ts} +tn {members}")
    try:
        replies = await cs.run(hub, admin, f"register {chan} Admin")
        assert any("has been registered" in line.lower() or " J " in line
                   for line in replies), replies
    except BaseException:
        unregister(chan)
        raise


def unregister(chan: str) -> None:
    """Every row a REGISTER of ``chan`` leaves in the shared database.

    PURGE only takes the registration off; the rows stay, and the database
    outlives the test session.
    """
    ids = f"(SELECT id FROM channels WHERE name = '{chan}')"
    psql(f"DELETE FROM channellog WHERE channelID IN {ids};"
         f"DELETE FROM bans WHERE channel_id IN {ids};"
         f"DELETE FROM levels WHERE channel_id IN {ids};"
         f"DELETE FROM channels WHERE name = '{chan}'", db="cservice")


@pytest.mark.asyncio
async def test_cservice_acts_on_a_join_to_a_registered_channel(cservice_linked):
    """EVT_JOIN: a client matching one of the channel's bans is kicked, and the
    ban goes on the channel first.  EVT_BURST takes the same path for a client
    a bursting server brings in."""
    hub, proc = cservice_linked
    chan = "#" + unique("csjoin")
    admin = await cs.login(hub)
    ts = int(time.time()) - 3600
    await _register(hub, admin, chan, admin, ts)
    x = cs.numnick(hub)

    try:
        await cs.run(hub, admin, f"ban {chan} *!*@drone.example 1h 75 not you")

        # EVT_JOIN
        after = len(hub.received)
        joiner = await hub.introduce_nick("joiner", username="joiner", host="drone.example")
        await hub.send_raw(f"{joiner} J {chan} {ts}")
        kick = await hub.wait_for(
            lambda line: p10_token(line) == "K" and f" {chan} {joiner}" in line,
            timeout=15.0, after=after)
        assert kick.startswith(f"{x} K {chan} {joiner} :")

        # EVT_BURST: the same client, brought in by a bursting server
        leaf = await hub.introduce_leaf("burst.testnet", 43, protocol="J11")
        burster = await hub.introduce_leaf_nick(leaf, 43, "burster", host="drone.example")
        after = len(hub.received)
        await hub.send_raw(f"{leaf} B {chan} {ts} {burster}")
        kick = await hub.wait_for(
            lambda line: p10_token(line) == "K" and f" {chan} {burster}" in line,
            timeout=15.0, after=after)
        assert kick.startswith(f"{x} K {chan} {burster} :")
        await hub.end_burst(leaf)
        alive(proc)
    finally:
        await cs.run(hub, admin, f"unban {chan} *!*@drone.example")
        await cs.run(hub, admin, f"purge {chan} pinning events")
        unregister(chan)


@pytest.mark.asyncio
async def test_cservice_sets_a_registered_channel_back_to_plus_r_when_it_is_created(
        cservice_linked):
    """EVT_CREATE: a registered channel X is not in, created again by somebody
    joining it, is set +R with the time it was registered with.

    The one wire line this file asserts that does NOT name the module's own
    numnick: cservice sends this mode with a null acting client, so its source
    is gnuworld's SERVER numeric.
    """
    hub, proc = cservice_linked
    chan = "#" + unique("cscreate")
    admin = await cs.login(hub)
    ts = int(time.time()) - 3600
    await _register(hub, admin, chan, admin, ts)
    srv = hub.peer_numeric

    try:
        # Empty the channel: X parts, then its only other member does
        await cs.run(hub, admin, f"part {chan}")
        await hub.send_raw(f"{admin} L {chan}")
        await hub.drain_messages(timeout=1.0)

        after = len(hub.received)
        maker = await hub.introduce_nick("maker", username="maker")
        await hub.send_raw(f"{maker} C {chan} {int(time.time())}")
        line = await hub.wait_for(
            lambda l: p10_token(l) == "M" and f" {chan} +R" in l, timeout=15.0, after=after)
        assert line.split(" ")[0] == srv, line
        alive(proc)
    finally:
        await cs.run(hub, admin, f"purge {chan} pinning events")
        unregister(chan)


@pytest.mark.asyncio
async def test_cservice_sees_a_part_of_a_registered_channel(docker_stack, fake_hub_p11, tmp_path):
    """EVT_PART: X's only unconditional consequence is the record it writes,
    at TRACE on its own logger; the flood protection behind it needs FLOODPRO
    configured.  A logging.conf that turns TRACE on puts that record on the
    console, which is as cheaply as a part can be seen at all."""
    async with link_cservice_logging(
        docker_stack, fake_hub_p11, tmp_path,
        logging_conf="sink.console.type = console\nlogger.root = TRACE, console\n",
    ) as (hub, proc, _conf_dir):
        chan = "#" + unique("cspart")
        admin = await cs.login(hub)
        ts = int(time.time()) - 3600
        await _register(hub, admin, chan, admin, ts)
        try:
            parter = await hub.introduce_nick("parter", username="parter")
            await hub.send_raw(f"{parter} J {chan} {ts}")
            await hub.send_raw(f"{parter} L {chan} :so long")
            await proc.wait_for_stdout(
                re.compile(rf"parter Part {re.escape(chan)} \(so long\)"), timeout=20.0)
            alive(proc)
        finally:
            await cs.run(hub, admin, f"purge {chan} pinning events")
            unregister(chan)


@pytest.mark.asyncio
async def test_cservice_takes_an_account_for_a_client_it_never_saw_arrive(
        docker_stack, fake_hub_p11, tmp_path):
    """EVT_ACCOUNT for a client that was already on the network at OnAttach().

    X hangs its per-client record off the client in its EVT_NICK handler, so a
    client whose N was posted before X registered for EVT_NICK has none.  A
    module ahead of X in GNUWorld.conf has exactly such a client:
    xServer::AttachClient() posts the N for a module's own client, and it
    attaches that module first.  An AC for that client then reaches X's account
    handler with no record to write the account through.
    """
    require_module("cservice")
    require_module("gnutest")
    require_module("debug")
    docker_stack.up()  # Postgres
    hub = fake_hub_p11
    conf_dir = _prepare_conf_dir(tmp_path)
    root = GnuworldProc.conf_root(conf_dir)
    GnuworldProc.write_gnutest_config(conf_dir / "gnutest.conf")
    GnuworldProc.write_cservice_config(conf_dir / "cservice.conf")
    GnuworldProc.write_debug_config(conf_dir / "debug.conf")
    GnuworldProc.write_config(
        conf_dir / "GNUWorld.conf",
        uplink=CONTAINER_UPLINK,
        port=hub.port,
        password=hub.password,
        # mod.gnutest first: its client is on the network before X attaches
        module_lines=f"module = libgnutest.la {root}/gnutest.conf\n"
        f"module = libcservice.la {root}/cservice.conf\n"
        f"module = libdebug.la {root}/debug.conf",
    )

    proc = GnuworldProc(conf_dir=conf_dir)
    await proc.start()
    try:
        await hub.accept_and_handshake(timeout=90.0)
        await proc.wait_for_stdout("Connected", timeout=60.0)
        assert hub.get_user_numnick("X"), "mod.cservice did not introduce X"
        older = hub.get_user_numnick("gnutest")
        assert older, "mod.gnutest did not introduce a client of its own"

        # The account has to be one X can look up, or its handler stops before
        # the record it would have written through
        admin = await cs.login(hub)
        assert await authed_nicks(hub, admin) == {"adminone"}

        # X's one consequence here is that nothing happens: there is no record
        # to put the account in.  A crash is what used to happen instead, so
        # let the process be reaped and read alive() before the wire, which
        # would otherwise report a link that closed and not why.
        await hub.send_account(older, "Admin")
        await asyncio.sleep(1.0)
        alive(proc)
        assert await authed_nicks(hub, admin) == {"adminone"}
    finally:
        await proc.terminate()


# ==========================================================================
# mod.ccontrol
# ==========================================================================


async def cc_curusers(hub, oper: str) -> int:
    """The counter EVT_NICK raises and EVT_QUIT/EVT_KILL lower, as MAXUSERS
    reports it."""
    for line in await cc.run(hub, oper, "maxusers"):
        text = _reply_to(cc.numnick(hub), oper, line)
        if text and text.startswith("Current number of users is: "):
            return int(text.rsplit(" ", 1)[1])
    raise AssertionError("mod.ccontrol did not answer MAXUSERS")


async def cc_status(hub, oper: str) -> list[str]:
    return [text for text in
            (_reply_to(cc.numnick(hub), oper, line) for line in await cc.run(hub, oper, "status"))
            if text]


@pytest.mark.asyncio
async def test_ccontrol_counts_the_clients_the_network_brings_and_takes(ccontrol_linked):
    """EVT_NICK, EVT_QUIT and EVT_KILL: euworld keeps a count of the clients on
    the network and MAXUSERS reports it."""
    hub, proc = ccontrol_linked
    oper = await cc.login(hub)
    before = await cc_curusers(hub, oper)

    alice = await hub.introduce_nick("alice", username="alice")
    bob = await hub.introduce_nick("bob", username="bob")
    assert await cc_curusers(hub, oper) == before + 2

    await hub.send_raw(f"{alice} Q :gone")
    assert await cc_curusers(hub, oper) == before + 1

    await hub.send_raw(f"{hub.server_numnick} D {bob} :hub.testnet!oper (bye)")
    assert await cc_curusers(hub, oper) == before
    alive(proc)


@pytest.mark.asyncio
async def test_ccontrol_follows_the_network_splitting_and_joining(ccontrol_linked):
    """EVT_NETJOIN (a server it does not know is announced on its log channel,
    and it goes into burst), EVT_BURST_CMPLT and EVT_NETBREAK (each works out
    again whether anything is still bursting)."""
    hub, proc = ccontrol_linked
    oper = await cc.login(hub)
    log_chan, bot = "#gnuworld.message", cc.numnick(hub)
    assert "Currently Bursting: NO" in await cc_status(hub, oper)

    # EVT_NETJOIN
    after = len(hub.received)
    leaf = await hub.introduce_leaf("leaf1.testnet", 44)
    await console(hub, bot, log_chan, after, "Unknown server just connected: leaf1.testnet")
    assert "Currently Bursting: YES" in await cc_status(hub, oper)

    # EVT_BURST_CMPLT
    await hub.end_burst(leaf)
    assert "Currently Bursting: NO" in await cc_status(hub, oper)

    # EVT_NETBREAK
    second = await hub.introduce_leaf("leaf2.testnet", 45)
    assert "Currently Bursting: YES" in await cc_status(hub, oper)
    await hub.send_raw(f"{hub.server_numnick} SQ leaf2.testnet 0 :bye")
    assert "Currently Bursting: NO" in await cc_status(hub, oper)
    assert second  # the leaf's numeric is not needed again, but it was announced
    alive(proc)


@pytest.mark.asyncio
async def test_ccontrol_keeps_the_glines_the_network_sets_and_lifts(ccontrol_linked):
    """EVT_GLINE and EVT_REMGLINE: euworld keeps its own record of every gline
    and SCANGLINE lists them.  mod.ccontrol is the only module that handles
    both."""
    hub, proc = ccontrol_linked
    oper = await cc.login(hub)
    host = f"*@{unique('gl')}.example.invalid"

    def listed(replies: list[str]) -> list[str]:
        bot = cc.numnick(hub)
        return [t for t in (_reply_to(bot, oper, line) for line in replies)
                if t and t.startswith(f"Host: {host},")]

    try:
        assert listed(await cc.run(hub, oper, f"scangline {host}")) == []

        # "GL <target> +<mask> <duration> <lastmod> :<reason>"
        await hub.send_raw(
            f"{hub.server_numnick} GL * +{host} 3600 {int(time.time())} :pinning events")
        found = listed(await cc.run(hub, oper, f"scangline {host}"))
        assert len(found) == 1, found
        expires = int(re.search(r"Expires: \[(\d+)\]", found[0]).group(1))
        assert abs(expires - (time.time() + 3600)) < 120, found

        await hub.send_raw(f"{hub.server_numnick} GL * -{host}")
        assert listed(await cc.run(hub, oper, f"scangline {host}")) == []
        alive(proc)
    finally:
        psql(f"DELETE FROM glines WHERE host = '{host}'", db="ccontrol")


@pytest.mark.asyncio
async def test_ccontrol_notices_a_client_becoming_an_oper(ccontrol_linked):
    """EVT_OPER: euworld records the oper's nick!user@host against its IP, and
    says so on its console."""
    hub, proc = ccontrol_linked
    await cc.login(hub)
    nick = unique("newoper")
    client = await hub.introduce_nick(nick, username="newoper", host="oper.example")

    await hub.send_raw(f"{client} M {nick} +o")
    await proc.wait_for_stdout(
        re.compile(rf"ccontrol::isNowAnOper\(\)>\s+{nick}!newoper@oper\.example"), timeout=20.0)
    alive(proc)


@pytest.mark.asyncio
async def test_ccontrol_ops_an_oper_who_joins_an_oper_channel(ccontrol_linked):
    """EVT_JOIN and EVT_BURST on a channel from "operchan": euworld ops any
    oper it sees arrive."""
    hub, proc = ccontrol_linked
    await cc.login(hub)
    chan = "#valhalla"
    ts = burst_ts(hub, chan)

    # EVT_BURST
    burst_oper = await hub.introduce_nick("burstop", username="burstop", modes="+io")
    after = len(hub.received)
    await hub.send_raw(f"{hub.server_numnick} B {chan} {ts} +sin {burst_oper}")
    line = await hub.wait_for(
        lambda l: p10_token(l) == "M" and f" {chan} +o {burst_oper}" in l, timeout=15.0, after=after)
    assert line == f"{cc.numnick(hub)} M {chan} +o {burst_oper} {ts}", line

    # EVT_JOIN
    join_oper = await hub.introduce_nick("joinop", username="joinop", modes="+io")
    after = len(hub.received)
    await hub.send_raw(f"{join_oper} J {chan} {ts}")
    await hub.wait_for(lambda l: p10_token(l) == "M" and f" {chan} +o {join_oper}" in l,
                       timeout=15.0, after=after)
    alive(proc)


@pytest.fixture
def ccontrol_sso_account():
    """Gives the harness's oper an X account to be signed in by.

    mod.ccontrol builds its account map when it loads, so this has to be in
    the database before gnuworld starts - hence a fixture, listed first.  The
    column is always put back: the database is shared for the whole session.
    """
    require_module("ccontrol")
    psql("UPDATE opers SET account = 'Admin' WHERE user_name = 'Admin'", db="ccontrol")
    try:
        yield "Admin"
    finally:
        psql("UPDATE opers SET account = NULL WHERE user_name = 'Admin'", db="ccontrol")


@pytest.mark.asyncio
async def test_ccontrol_signs_in_an_oper_whose_account_arrives(ccontrol_sso_account,
                                                               docker_stack, fake_hub, tmp_path):
    """EVT_ACCOUNT: an AC for an account euworld has an oper record for signs
    that oper in, with no LOGIN."""
    async with link_ccontrol_logging(docker_stack, fake_hub, tmp_path) as (hub, proc, _conf):
        oper = await hub.introduce_nick("ssooper", username="ssooper", modes="+io")
        after = len(hub.received)
        await hub.send_account(oper, ccontrol_sso_account)
        line = await hub.wait_for(
            lambda l: (_reply_to(cc.numnick(hub), oper, l) or "").startswith(
                "Authentication successful as Admin"),
            timeout=20.0, after=after)
        assert " :Authentication successful as Admin" in strip_msg_tags(line)
        alive(proc)


# ==========================================================================
# mod.dronescan
# ==========================================================================


DRONESCAN_TUNING = {
    # A channel is only looked at above channelCutoff members, and the flood
    # counters only warn at their cutoffs: the defaults (10 each) need more
    # clients and more traffic than a test wants.  The behaviour pinned is the
    # module's, not these numbers'.
    "channelCutoff": "3",
    "jcCutoff": "2",
    "jcInterval": "3",
    "ncCutoff": "2",
    "ncInterval": "3",
    "jcGracePeriodBurstOrSplit": "0",
    # Its console level, so that what it logs at DEBUG is on the channel too
    "consoleLevel": "0",
}

_HARNESS_DB = {"host": "127.0.0.1", "port": "5433", "user": "gnuworld", "password": "gnuworld"}


@asynccontextmanager
async def dronescan_tuned(docker_stack, hub, tmp_path, **overrides):
    settings = {"sqlHost": _HARNESS_DB["host"], "sqlPort": _HARNESS_DB["port"],
                "sqlDB": "dronescan", "sqlUser": _HARNESS_DB["user"],
                "sqlPass": _HARNESS_DB["password"], **DRONESCAN_TUNING, **overrides}
    async with link_module(docker_stack, hub, tmp_path, "dronescan", "libdronescan.la",
                           "dronescan.example.conf", settings) as linked:
        yield linked


async def ds_custom_data(hub, oper: str) -> int:
    """The per-client records EVT_NICK makes and EVT_QUIT/EVT_KILL free, as
    dronescan's own STATS reports them."""
    e = hub.get_user_numnick("E")
    for text in await ask(hub, oper, e, "STATS", "ncI/ncC"):
        if text.startswith("Allocated custom data: "):
            return int(text.rsplit(" ", 1)[1])
    raise AssertionError("mod.dronescan did not answer STATS")


@pytest.mark.asyncio
async def test_dronescan_counts_the_clients_it_has_a_record_for(dronescan_linked):
    """EVT_NICK, EVT_QUIT and EVT_KILL: one clientData per client on the
    network, counted by the module itself."""
    hub, proc = dronescan_linked
    oper = await hub.introduce_nick("dsoper", username="dsoper", modes="+io")
    before = await ds_custom_data(hub, oper)

    alice = await hub.introduce_nick("alice", username="alice")
    bob = await hub.introduce_nick("bob", username="bob")
    assert await ds_custom_data(hub, oper) == before + 2

    await hub.send_raw(f"{alice} Q :gone")
    assert await ds_custom_data(hub, oper) == before + 1

    await hub.send_raw(f"{hub.server_numnick} D {bob} hub.testnet!oper :(bye)")
    assert await ds_custom_data(hub, oper) == before
    alive(proc)


@pytest.mark.asyncio
async def test_dronescan_bursts_and_runs_as_servers_come_and_go(dronescan_linked):
    """EVT_NETJOIN puts dronescan into BURST; EVT_BURST_CMPLT and EVT_NETBREAK
    each work out whether anything is still bursting and put it back in RUN.
    It says so itself as it changes state."""
    hub, proc = dronescan_linked
    burst, run = "DroneScan: Entering state BURST", "DroneScan: Entering state RUN"
    # It has already been through its own link burst once
    bursts, runs = said(proc, burst), said(proc, run)

    # EVT_NETJOIN
    first = await hub.introduce_leaf("dsleaf1.testnet", 46, protocol="J11")
    await wait_said(proc, burst, bursts + 1)

    # EVT_BURST_CMPLT
    await hub.end_burst(first)
    await wait_said(proc, run, runs + 1)

    # EVT_NETBREAK: into BURST again, then squitted away while still bursting
    await hub.introduce_leaf("dsleaf2.testnet", 47, protocol="J11")
    await wait_said(proc, burst, bursts + 2)
    await hub.send_raw(f"{hub.server_numnick} SQ dsleaf2.testnet 0 :bye")
    await wait_said(proc, run, runs + 2)
    alive(proc)


@pytest.mark.asyncio
async def test_dronescan_ops_an_oper_who_joins_its_console_channel(dronescan_linked):
    """EVT_JOIN, the branch before the burst and channel-size gates: an oper
    joining dronescan's console channel is opped whatever state it is in."""
    hub, proc = dronescan_linked
    chan = "#ds.console"
    ts = burst_ts(hub, chan)
    e = hub.get_user_numnick("E")

    after = len(hub.received)
    oper = await hub.introduce_nick("dsconsole", username="dsconsole", modes="+io")
    await hub.send_raw(f"{oper} J {chan} {ts}")
    line = await hub.wait_for(
        lambda l: p10_token(l) == "M" and f" {chan} +o " in l, timeout=20.0, after=after)
    assert strip_msg_tags(line) == f"{e} M {chan} +o {oper} {ts}", line
    alive(proc)


@pytest.mark.asyncio
async def test_dronescan_watches_joins_and_parts_but_not_nick_changes(docker_stack, fake_hub_p11,
                                                                      tmp_path):
    """EVT_JOIN warns on dronescan's console once a channel has seen enough
    joins; EVT_PART is counted, and the count is in the message the join flood
    ends with.

    EVT_CHNICK is handled too (handleNickChange, which warns the same way once
    a channel has seen enough nick changes) but OnAttach() never registers for
    the event, so that handler is dead code.  The nick changes at the end pin
    that, on the same channel and the same running dronescan whose join and
    part counting has just been seen: a handleNickChange() the core started
    calling would be new behaviour, not a move.
    """
    async with dronescan_tuned(docker_stack, fake_hub_p11, tmp_path) as (hub, proc):
        chan, ts = "#" + unique("dsflood"), int(time.time()) - 3600
        e = hub.get_user_numnick("E")
        seed = [await hub.introduce_nick(f"seed{i}", username=f"seed{i}") for i in range(3)]
        await hub.send_raw(f"{hub.server_numnick} B {chan} {ts} +tn {','.join(seed)}")
        await hub.drain_messages(timeout=1.0)

        # EVT_JOIN
        after = len(hub.received)
        joiners = [await hub.introduce_nick(f"join{i}", username=f"join{i}") for i in range(3)]
        for numnick in joiners:
            await hub.send_raw(f"{numnick} J {chan} {ts}")
        await console(hub, e, "#ds.console", after, f"{chan} is being join flooded.")

        # EVT_PART: counted on a channel that is flooding, and reported when
        # the flood is over
        after = len(hub.received)
        for numnick in joiners:
            await hub.send_raw(f"{numnick} L {chan} :bye")
        over = await console(hub, e, "#ds.console", after, f"Join flood over in {chan}.")
        parts = int(re.search(r"Total parts: (\d+)", over).group(1))
        assert parts == len(joiners), over

        # EVT_CHNICK, which nothing delivers: the same channel still has
        # channelCutoff members, and more nick changes than ncCutoff change
        # nothing on the console
        after = len(hub.received)
        for index, numnick in enumerate(seed):
            await hub.send_raw(f"{numnick} N renamed{index} {int(time.time())}")
        with pytest.raises(TimeoutError):
            await console(hub, e, "#ds.console", after, "is being nick flooded.", timeout=8.0)
        alive(proc)


# ==========================================================================
# mod.openchanfix
# ==========================================================================


CHANFIX_DB = {"sqlHost": _HARNESS_DB["host"], "sqlPort": _HARNESS_DB["port"], "sqlDB": "chanfix",
              "sqlcfUser": _HARNESS_DB["user"], "sqlPass": _HARNESS_DB["password"]}


@asynccontextmanager
async def chanfix_tuned(docker_stack, hub, tmp_path, **overrides):
    async with link_module(docker_stack, hub, tmp_path, "openchanfix", "libopenchanfix.la",
                           "openchanfix.example.conf", {**CHANFIX_DB, **overrides}) as linked:
        yield linked


async def chanfix_oper(hub, nick: str = "cfoper") -> str:
    """An oper with an account: chanfix refuses every command to a client that
    has none."""
    oper = await hub.introduce_nick(nick, username=nick, modes="+io")
    await hub.send_account(oper, PERMIT_ACCOUNT)
    return oper


async def chanfix_status(hub, oper: str) -> list[str]:
    c = hub.get_user_numnick("C")
    return await ask(hub, oper, c, "STATUS", "Memory Usage")


def chanfix_forget(*channels: str) -> None:
    """Every row chanfix writes for a channel and every command it logged.

    Called once gnuworld has stopped: its sqlManager flushes what it has in
    memory on a timer, so a DELETE while it is still running can be written
    straight back.
    """
    psql(f"DELETE FROM comlog WHERE user_name = '{PERMIT_ACCOUNT}'", db="chanfix")
    for chan in channels:
        psql(f"DELETE FROM chanops WHERE channel = '{chan}';"
             f"DELETE FROM channels WHERE channel = '{chan}'", db="chanfix")


async def chanfix_score(hub, oper: str, chan: str, account: str) -> str:
    """What chanfix says about one account's score on one channel.

    Either "Score for account ... (Ranked #n of m)", "No score for account ..."
    or "There are no scores in the database for ...", whichever it answers.
    """
    c = hub.get_user_numnick("C")
    replies = await ask(hub, oper, c, f"SCORE {chan} {account}",
                        ("Score for account", "No score for account",
                         "no scores in the database"))
    return replies[-1]


@pytest.mark.asyncio
async def test_openchanfix_follows_the_network_splitting_and_joining(openchanfix_linked):
    """EVT_NETJOIN, EVT_NETBREAK and EVT_BURST_CMPLT: chanfix will not score or
    fix anything while too little of the network is linked, and STATUS names
    the state it is in and whether it has seen the channel service.

    numServers is 10 and minServersPresent 75%, so 8 servers are the minimum;
    the leaves below cross that line in both directions.
    """
    hub, proc = openchanfix_linked
    oper = await chanfix_oper(hub)
    try:
        status = await chanfix_status(hub, oper)
        assert any(t.startswith("Splitmode disabled.") for t in status), status
        assert "Channel service not linked. New channels will not be scored." in status

        # EVT_NETJOIN, below the minimum: into SPLIT.  This leaf is the
        # channel service of the configuration, so it is noticed twice.
        # (60 upwards: 1 is the hub's numeric and 51 is gnuworld's own, and a
        # leaf announced with either of those is dropped)
        await hub.introduce_leaf("channels.undernet.org", 60)
        status = await chanfix_status(hub, oper)
        assert any(t.startswith("Splitmode enabled: only 3 servers linked.") for t in status), status
        assert "Channel service linked. New channels will be scored." in status

        # EVT_NETJOIN, over the minimum: out of SPLIT and into BURST, where it
        # takes no commands at all
        leaves = [await hub.introduce_leaf(f"cfleaf{i}.testnet", 61 + i) for i in range(5)]
        c = hub.get_user_numnick("C")
        busy = await ask(hub, oper, c, "STATUS",
                         ("try again soon", "during a burst", "Memory Usage"))
        assert any("too busy" in t or "during a burst" in t for t in busy), busy

        # EVT_BURST_CMPLT: back to RUN, and answering again
        await hub.end_burst(leaves[0])
        status = await chanfix_status(hub, oper)
        assert any(t.startswith("Splitmode disabled. There are 8 servers linked.") for t in status)

        # EVT_NETBREAK, the channel service leaving: it will not score a
        # channel that has no scores yet any more.  Its own count is still 8
        # here, because the core removes the split server only after the event,
        # so this break alone does not put it back into SPLIT.
        await hub.send_raw(f"{hub.server_numnick} SQ channels.undernet.org 0 :bye")
        status = await chanfix_status(hub, oper)
        assert "Channel service not linked. New channels will not be scored." in status
        assert any(t.startswith("Splitmode disabled. There are 7 servers linked.") for t in status)

        # EVT_NETBREAK, one more: now it counts 7 and goes back into SPLIT
        await hub.send_raw(f"{hub.server_numnick} SQ cfleaf1.testnet 0 :bye")
        status = await chanfix_status(hub, oper)
        assert any(t.startswith("Splitmode enabled: only 6 servers linked.") for t in status), status
        alive(proc)
    finally:
        # Only the command log: chanfix writes those rows as it goes, not on
        # its commit timer, and no command follows this
        psql(f"DELETE FROM comlog WHERE user_name = '{PERMIT_ACCOUNT}'", db="chanfix")


@pytest.mark.asyncio
async def test_openchanfix_answers_an_xquery_about_a_channel_with_no_scores(openchanfix_linked):
    """EVT_XQUERY: OPLIST, which chanfix answers with an XR of its own.

    The negative answer costs nothing to set up and nothing to clean up: a
    channel nobody has been opped in has no scores, and the branch that says so
    (chanfix.cc, doXROplist) only reads the in-memory score map and the channel
    cache.  Nor is there a state gate on the event, so this holds in SPLIT,
    which is where chanfix is on a link of two servers.

    The reply comes from gnuworld's SERVER numeric, like every XR: xServer
    sends it, and the module is only the thing that asked for it.
    """
    hub, proc = openchanfix_linked
    chan = "#" + unique("cfxq")
    routing = unique("oplist:")

    after = len(hub.received)
    await hub.send_xquery(routing=routing, message=f"OPLIST {chan}")
    line = await hub.wait_for(
        lambda l: p10_token(l) == "XR" and routing in l, timeout=15.0, after=after)
    assert strip_msg_tags(line) == (
        f"{hub.peer_numeric} XR {hub.server_numnick} {routing} :OPLIST {chan} NO"), line
    alive(proc)


@pytest.mark.asyncio
async def test_openchanfix_drops_a_client_that_quits_from_its_op_tracking(docker_stack,
                                                                         fake_hub_p11, tmp_path):
    """EVT_ACCOUNT, EVT_NICK, EVT_QUIT, and OnChannelModeO (chanfix's op
    tracking, not part of this rewrite's EVT_* surface).

    chanfix keeps every logged-in client in a map by account, and every channel
    a client is opped in on the client itself.  The second of those is filled by
    OnChannelModeO, an already-typed virtual of its own, which the raw M line
    below triggers; chanfix's EVT_JOIN/EVT_BURST case only auto-ops opers on its
    own joinChans list, and this channel is not one of those, so no EVT_ handler
    is what puts the client in the opped set.  EVT_QUIT walks both, and
    asserts the client is in the list its own account maps to
    (chanfix.cc:866) - asserts are live in this build, so this makes that path
    run with a client that really is in both structures.

    numServers is dropped to 1 so that a leaf joining does not put chanfix into
    SPLIT, where it scores nothing: the channel service has to be linked before
    a channel with no scores gets any.
    """
    chan, ts = "#" + unique("cfops"), int(time.time()) - 3600
    try:
        async with chanfix_tuned(docker_stack, fake_hub_p11, tmp_path,
                                 numServers="1") as (hub, proc):
            oper = await chanfix_oper(hub)
            await hub.introduce_leaf("channels.undernet.org", 60)
            status = await chanfix_status(hub, oper)
            assert "Channel service linked. New channels will be scored." in status

            # EVT_NICK for one (introduced with its account), EVT_ACCOUNT for
            # the others: each goes into the map keyed by account
            account = unique("CFOP")
            opped = await hub.introduce_nick("cfopped", username="cfopped",
                                             modes=f"+ir {account}")
            others = []
            for index in range(3):
                nick = f"cfuser{index}"
                numnick = await hub.introduce_nick(nick, username=nick)
                await hub.send_account(numnick, f"{account}{index}")
                others.append(numnick)

            # chanfix only looks at a channel with minClients members
            await hub.send_raw(f"{hub.server_numnick} B {chan} {ts} +tn "
                               f"{','.join([opped] + others)}")
            await hub.drain_messages(timeout=1.0)
            assert "no scores in the database" in await chanfix_score(hub, oper, chan, account)

            # Opped, so chanfix now has a record of the account on the channel
            # and the channel on the client
            await hub.send_raw(f"{hub.server_numnick} M {chan} +o {opped} {ts}")
            scored = await chanfix_score(hub, oper, chan, account)
            assert scored.startswith(f"Score for account {account} in channel {chan}:"), scored

            # EVT_QUIT: the account map entry and the opped-channel set are
            # both non-empty for this client, which is what the assert at
            # chanfix.cc:866 is about
            await hub.send_raw(f"{opped} Q :gone")
            await asyncio.sleep(0.5)
            alive(proc)

            # Still answering, on the same data
            assert await chanfix_score(hub, oper, chan, account) == scored
            alive(proc)
    finally:
        chanfix_forget(chan)


@pytest.mark.asyncio
async def test_openchanfix_sees_an_op_kicked(docker_stack, fake_hub_p11, tmp_path):
    """OnNetworkKick, which this module gained so that a kicked client loses the
    op record a parting one has always lost.

    Core posts a kick by name and never through the numbered channel-event
    switch these handlers were converted from, so the EVT_KICK case that sat
    beside EVT_PART in that switch was dead from the day it was written: only
    the part half ever ran.

    Two kicks, because the handler's lookup runs for every kick on the network
    and not only for an op it knows: the first victim has no op record and the
    second has one.  Both are kicked out of a channel that is still at
    minClients afterwards, which is the size the gate the part path shares
    reads.  The client then quits, so the walk over the channels it is opped in
    runs with the kick already accounted for.

    What the kick costs the client is a set the module keeps on the client
    itself, and nothing outside the module reads it: gotOpped() skips an insert
    it has already made and lostClient() walks it to clear it, and neither tells
    anybody.  A command's answer about the channel comes from the core's own
    membership, and the score the module keeps for the account is not what a
    lost op touches.  So this test passes on the code that had no kick handler
    too, and says so rather than pretending otherwise: it pins the path, not the
    set.  OPNICKS names the opped client before the kick and none after, the
    lookup runs for a victim of each kind, and gnuworld is still running each
    time, which these paths do not always manage.

    numServers is dropped to 1 for the same reason as the test above: chanfix
    scores nothing in SPLIT, and it has to be out of it before a channel with
    no scores gets any.
    """
    chan, ts = "#" + unique("cfkick"), int(time.time()) - 3600
    try:
        async with chanfix_tuned(docker_stack, fake_hub_p11, tmp_path,
                                 numServers="1") as (hub, proc):
            oper = await chanfix_oper(hub, "cfkicker")
            c = hub.get_user_numnick("C")
            await hub.introduce_leaf("channels.undernet.org", 60)
            status = await chanfix_status(hub, oper)
            assert "Channel service linked. New channels will be scored." in status

            account = unique("CFKICK")
            opped = await hub.introduce_nick("cfvictim", username="cfvictim",
                                            modes=f"+ir {account}")
            # minClients is 4 and the victim is off the channel before the
            # module hears, so six go in and four are left after two kicks
            fillers = []
            for index in range(5):
                nick = f"cfstay{index}"
                fillers.append(await hub.introduce_nick(nick, username=nick))
            await hub.send_raw(f"{hub.server_numnick} B {chan} {ts} +tn "
                               f"{','.join([opped] + fillers)}")
            await hub.drain_messages(timeout=1.0)

            # A client the module has no op record for, which is every kick on
            # a real network bar a handful
            await hub.send_raw(f"{fillers[0]} K {chan} {fillers[1]} :nothing owed")
            await hub.drain_messages(timeout=1.0)
            alive(proc)

            # Opped, so the module has a score for the account and the channel
            # on the client
            await hub.send_raw(f"{hub.server_numnick} M {chan} +o {opped} {ts}")
            scored = await chanfix_score(hub, oper, chan, account)
            assert scored.startswith(f"Score for account {account} in channel {chan}:"), scored
            named = await ask(hub, oper, c, f"OPNICKS {chan}", "opped client")
            assert f"I see 1 opped client in {chan}." in named, named
            assert "cfvictim" in named, named

            # The kicker is on the victim's server, so the kick is
            # authoritative and the core has taken the member off already
            await hub.send_raw(f"{fillers[0]} K {chan} {opped} :out")
            await hub.drain_messages(timeout=1.0)
            alive(proc)
            gone = await ask(hub, oper, c, f"OPNICKS {chan}", "opped client")
            assert f"I see 0 opped clients in {chan}." in gone, gone

            # EVT_QUIT, with the kick accounted for: the score the module keeps
            # for the account is not what the kick took, so it is still there
            await hub.send_raw(f"{opped} Q :gone")
            await asyncio.sleep(0.5)
            alive(proc)
            assert await chanfix_score(hub, oper, chan, account) == scored
            alive(proc)
    finally:
        chanfix_forget(chan)


@pytest.mark.asyncio
async def test_openchanfix_ops_an_oper_who_joins_a_channel_it_is_in(docker_stack, fake_hub_p11,
                                                                    tmp_path):
    """EVT_JOIN on one of chanfix's own channels: an oper is opped.  EVT_KILL
    takes the same path as EVT_QUIT afterwards."""
    chan = "#chanfix"  # one of chanfix's own joinChans
    try:
        async with chanfix_tuned(docker_stack, fake_hub_p11, tmp_path,
                                 numServers="1") as (hub, proc):
            ts = burst_ts(hub, chan)
            # minClients is 4 and chanfix is one of them
            fillers = [await hub.introduce_nick(f"cffill{i}", username=f"cffill{i}")
                       for i in range(3)]
            await hub.send_raw(f"{hub.server_numnick} B {chan} {ts} +nt {','.join(fillers)}")
            await hub.drain_messages(timeout=1.0)

            after = len(hub.received)
            oper = await chanfix_oper(hub, "cfjoiner")
            await hub.send_raw(f"{oper} J {chan} {ts}")
            line = await hub.wait_for(
                lambda l: p10_token(l) == "M" and f" {chan} +o {oper}" in l,
                timeout=20.0, after=after)
            # C itself, not the server: chanfix ops with xClient::Op()
            assert strip_msg_tags(line) == (
                f"{hub.get_user_numnick('C')} M {chan} +o {oper} {ts}"), line

            await hub.send_raw(f"{hub.server_numnick} D {oper} hub.testnet!oper :(bye)")
            await asyncio.sleep(0.5)
            alive(proc)
    finally:
        # Chanfix's own Op() is a mode change it then sees, so it has scored the
        # oper on its own channel
        chanfix_forget(chan)


# ==========================================================================
# mod.nickserv
# ==========================================================================


@pytest.mark.asyncio
async def test_nickserv_takes_an_account_for_a_client_it_never_saw_arrive(
        docker_stack, fake_hub_p11, tmp_path):
    """EVT_ACCOUNT for a client that was already on the network at OnAttach().

    NS hangs its per-client netData off the client in its EVT_NICK handler, so
    a client whose N was posted before NS registered for EVT_NICK has none.  A
    module ahead of NS in GNUWorld.conf has exactly such a client:
    xServer::AttachClient() posts the N for a module's own client, and it
    attaches that module first.  An AC for that client then reaches NS's
    account handler with no netData to write the user record through.

    The sibling of test_cservice_takes_an_account_for_a_client_it_never_saw_arrive.
    """
    require_module("nickserv")
    require_module("gnutest")
    docker_stack.up()  # Postgres
    hub = fake_hub_p11
    conf_dir = _prepare_conf_dir(tmp_path)
    root = GnuworldProc.conf_root(conf_dir)
    GnuworldProc.write_gnutest_config(conf_dir / "gnutest.conf")
    GnuworldProc.write_module_config(
        conf_dir / "nickserv.conf", "nickserv.example.conf",
        {"dbHost": _HARNESS_DB["host"], "dbPort": _HARNESS_DB["port"], "dbDb": "nickserv",
         "dbUser": _HARNESS_DB["user"], "dbPass": _HARNESS_DB["password"]})
    GnuworldProc.write_config(
        conf_dir / "GNUWorld.conf",
        uplink=CONTAINER_UPLINK,
        port=hub.port,
        password=hub.password,
        # mod.gnutest first: its client is on the network before NS attaches
        module_lines=f"module = libgnutest.la {root}/gnutest.conf\n"
        f"module = libnickserv.la {root}/nickserv.conf",
    )

    proc = GnuworldProc(conf_dir=conf_dir)
    await proc.start()
    try:
        await hub.accept_and_handshake(timeout=90.0)
        await proc.wait_for_stdout("Connected", timeout=60.0)
        ns = hub.get_user_numnick("NS")
        assert ns, "mod.nickserv did not introduce NS (module or database?)"
        older = hub.get_user_numnick("gnutest")
        assert older, "mod.gnutest did not introduce a client of its own"

        # A client NS did see arrive, to ask something of it afterwards: NS
        # answers no command from a client that is not logged in
        asker = await hub.introduce_nick("nsasker", username="nsasker")
        await hub.send_account(asker, PERMIT_ACCOUNT)
        assert await ask(hub, asker, ns, "WHOAMI", "Account:")

        # NS's one consequence here is that nothing happens: there is no netData
        # to put the user record in.  A crash is what used to happen instead, so
        # let the process be reaped and read alive() before the wire, which would
        # otherwise report a link that closed and not why.
        await hub.send_account(older, PERMIT_ACCOUNT)
        await asyncio.sleep(1.0)
        alive(proc)
        assert await ask(hub, asker, ns, "WHOAMI", "Account:")
    finally:
        await proc.terminate()
