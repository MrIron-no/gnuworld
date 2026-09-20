"""What a handler may do to the dispatch it is running inside.

Two mod.gnutest instances register for everything, gnutest first and gnutest2
second, so that every event reaches gnutest first: gnutest acts from inside its
handler ("onevent <NAME> <action>") and gnutest2 is the witness of what that
leaves of the dispatch. Each report names the module that sent it, so the order
below is the order gnuworld wrote them in.

Today's dispatch is a bare walk of a std::list with no protection of any kind
(xServer::PostEvent, xServer::PostChannelEvent). A handler may therefore do
things that end the walk: unregistering itself erases the node the iterator
stands on, killing the client the event is about leaves every later handler a
deleted iClient, emptying the channel leaves them a deleted Channel, and
detaching itself deletes the handler's own object. Each of those tests asserts
what the dispatch is meant to do and is marked xfail non-strict, because today
the behaviour is undefined: some of it happens not to crash, and some of it
kills the daemon. Every test checks that gnuworld is still running afterwards,
so an abort cannot read as a pass.
"""

from __future__ import annotations

import asyncio
import time

import pytest

import gnutest_client as gt
from p10 import p10_token, strip_msg_tags

CHAN = "#order"

# A deleted payload handed to the next handler in the same dispatch
UB_DELETED = (
    "UB today: nested dispatch hands module 2 a deleted client; "
    "fixed by events-dispatch-queue"
)
# A deleted channel handed to the next handler in the same dispatch
UB_DELETED_CHAN = (
    "UB today: nested dispatch hands module 2 a deleted channel; "
    "fixed by events-dispatch-queue"
)
# A change to the list the dispatch is walking
UB_ERASED = (
    "UB today: a handler that leaves the listener list erases the node the dispatch "
    "stands on and gnuworld dies; fixed by events-dispatch-queue"
)


def alive(proc) -> None:
    assert proc.proc is not None and proc.proc.returncode is None, "gnuworld died"


def numnick(hub, nick: str) -> str:
    found = hub.get_user_numnick(nick)
    assert found, f"gnuworld did not introduce {nick}"
    return found


async def command(hub, asker: str, nick: str, text: str, timeout: float = 10.0) -> list[str]:
    """gnutest_client.run() for whichever of the two modules is addressed."""
    target = numnick(hub, nick)
    after = len(hub.received)
    await hub.send_privmsg(asker, target, text)
    await hub.send_privmsg(asker, target, "sync")

    def is_sentinel(line: str) -> bool:
        return p10_token(line) == "O" and f" {asker} :" in line and gt.SENTINEL_REPLY in line

    await hub.wait_for(is_sentinel, timeout=timeout, after=after)

    out = []
    for line in hub.received[after:]:
        if is_sentinel(line):
            break
        out.append(strip_msg_tags(line))
    return out


async def setup(hub, channel_members: list[str] | None = None) -> dict[str, str]:
    """Three clients to be the subject of events, and CHAN if a test wants it.

    Everything exists before reporting is turned on, so that only what a test
    drives is reported. gnutest registers before gnutest2 and is therefore
    first in every event's list of listeners.
    """
    env = {
        "asker": await hub.introduce_nick("asker", username="asker"),
        "victim": await hub.introduce_nick("victim", username="victim"),
        "other": await hub.introduce_nick("other", username="other"),
        "third": await hub.introduce_nick("third", username="third"),
    }
    if channel_members is not None:
        await hub.burst_channel(
            CHAN, members=[env[name] + ":o" for name in channel_members],
            ts=int(time.time()) - 3600,
        )
    await command(hub, env["asker"], "gnutest", "events on")
    await command(hub, env["asker"], "gnutest2", "events on")
    return env


async def drive(hub, env: dict[str, str], lines: list[str], timeout: float = 10.0) -> list[str]:
    """Send these lines and return everything gnuworld wrote for them. The
    message that ends the window goes to gnutest2, which no test unloads."""
    asker = env["asker"]
    after = len(hub.received)
    for line in lines:
        await hub.send_raw(line)
    await hub.send_privmsg(asker, numnick(hub, "gnutest2"), "sync")

    def is_sentinel(line: str) -> bool:
        return p10_token(line) == "O" and f" {asker} :" in line and gt.SENTINEL_REPLY in line

    await hub.wait_for(is_sentinel, timeout=timeout, after=after)

    out = []
    for line in hub.received[after:]:
        if is_sentinel(line):
            break
        out.append(strip_msg_tags(line))
    return out


async def drive_or_die(hub, env, lines, proc, timeout: float = 10.0) -> list[str]:
    """drive(), but when the dispatch takes the daemon with it, say that and not
    that the socket closed."""
    try:
        return await drive(hub, env, lines, timeout=timeout)
    except (ConnectionError, OSError, TimeoutError):
        await asyncio.sleep(1.0)
        alive(proc)
        raise


def reports(hub, lines: list[str], env: dict[str, str]) -> list[tuple[str, str]]:
    """(module, report) for every EVENT notice, in the order gnuworld wrote it.

    EVT_RAW is posted for every line the uplink sends, so the only Raw kept is
    the one "onevent post" posts itself: that one is a nested event, which is
    what these tests are about.
    """
    out = []
    marker = f" {env['asker']} :EVENT "
    sources = {numnick(hub, nick): nick for nick in ("gnutest", "gnutest2")}
    for line in lines:
        if p10_token(line) != "O" or marker not in line:
            continue
        source = line.split(" ", 1)[0]
        if source not in sources:
            continue
        report = line.split(marker, 1)[1]
        if report.startswith("Raw ") and report != "Raw onevent post":
            continue
        out.append((sources[source], report))
    return out


@pytest.mark.asyncio
async def test_both_modules_report_every_event(two_gnutests_linked_p11):
    """The ground the rest stands on: the same library is loaded twice, each
    instance registers for itself, and every event reaches gnutest before
    gnutest2 because gnutest registered first."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub)

    out = await drive(hub, env, [f"{env['victim']} Q :bye now"])
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest", "Quit victim bye now"),
        ("gnutest2", "Quit victim bye now"),
    ]


@pytest.mark.xfail(strict=False, reason=UB_ERASED)
@pytest.mark.asyncio
async def test_a_handler_that_unregisters_itself(two_gnutests_linked_p11):
    """gnutest unregisters itself from inside the handler, which erases the very
    node xServer::PostEvent() has its iterator on: the walk then steps through a
    freed node. The event is still on its way to gnutest2, and from then on it
    should reach gnutest2 alone."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub)

    await command(hub, env["asker"], "gnutest", "onevent Quit unregister")

    out = await drive_or_die(hub, env, [f"{env['victim']} Q :bye now"], proc)
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest", "Quit victim bye now"),
        ("gnutest2", "Quit victim bye now"),
    ]

    out = await drive_or_die(hub, env, [f"{env['other']} Q :me too"], proc)
    alive(proc)
    assert reports(hub, out, env) == [("gnutest2", "Quit other me too")]


@pytest.mark.asyncio
async def test_a_handler_that_registers_an_event_again(two_gnutests_linked_p11):
    """RegisterEvent() removes the client before it adds it back
    (xServer::RegisterEvent), so registering from inside a handler moves that
    module to the end of the listener list: the order the two modules see the
    event in is reversed from then on. Registering for an event other than the
    one being dispatched touches no list the dispatch is walking, so this is
    well defined today."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub)

    await command(hub, env["asker"], "gnutest", "onevent NickChange register Quit")
    out = await drive(hub, env, [f"{env['victim']} N renamed {int(time.time())}"])
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest", "NickChange renamed victim"),
        ("gnutest2", "NickChange renamed victim"),
    ]

    out = await drive(hub, env, [f"{env['other']} Q :bye now"])
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest2", "Quit other bye now"),
        ("gnutest", "Quit other bye now"),
    ]


@pytest.mark.asyncio
async def test_a_handler_that_posts_a_nested_event(two_gnutests_linked_p11):
    """A nested event is dispatched to completion from inside the outer one, so
    gnutest2 sees the inner event before the outer one it was posted from."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub)

    await command(hub, env["asker"], "gnutest", "onevent Quit post")

    out = await drive(hub, env, [f"{env['victim']} Q :bye now"])
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest", "Quit victim bye now"),
        ("gnutest", "Raw onevent post"),
        ("gnutest2", "Raw onevent post"),
        ("gnutest2", "Quit victim bye now"),
    ]


@pytest.mark.xfail(strict=False, reason=UB_DELETED)
@pytest.mark.asyncio
async def test_a_handler_that_kills_the_client_the_event_is_about(two_gnutests_linked_p11):
    """gnutest kills the joining client from inside the join, which deletes the
    iClient and the ChannelUser that are the event's payload. gnutest2 is next
    in the same dispatch and is handed both."""
    hub, proc = two_gnutests_linked_p11
    # other stays behind, so that the channel is not emptied as well
    env = await setup(hub, channel_members=["other"])

    await command(hub, env["asker"], "gnutest", "onevent ChannelJoin kill")

    out = await drive_or_die(
        hub, env, [f"{env['victim']} J {CHAN} {int(time.time())}"], proc
    )
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest", f"ChannelJoin {CHAN} victim victim"),
        ("gnutest", "Kill - victim onevent kill"),
        ("gnutest2", "Kill - victim onevent kill"),
        ("gnutest2", f"ChannelJoin {CHAN} victim victim"),
    ]


@pytest.mark.xfail(strict=False, reason=UB_DELETED_CHAN)
@pytest.mark.asyncio
async def test_a_handler_that_empties_the_channel_the_event_is_about(two_gnutests_linked_p11):
    """gnutest kicks the only member of the channel the event is about, which
    deletes the Channel (xServer::kickMembers parts the module it joined to kick
    with, and a channel left empty goes with it). gnutest2 is next in the same
    dispatch and is handed that Channel."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub)

    await command(hub, env["asker"], "gnutest", "onevent ChannelCreate kick")

    out = await drive_or_die(
        hub, env, [f"{env['victim']} C {CHAN} {int(time.time())}"], proc
    )
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest", f"ChannelCreate {CHAN} victim victim"),
        ("gnutest", f"ChannelJoin {CHAN} gnutest gnutest"),
        ("gnutest", f"ChannelPart {CHAN} gnutest -"),
        ("gnutest2", f"ChannelJoin {CHAN} gnutest gnutest"),
        ("gnutest2", f"ChannelPart {CHAN} gnutest -"),
        ("gnutest2", f"ChannelCreate {CHAN} victim victim"),
    ]


@pytest.mark.asyncio
async def test_a_handler_that_parts_a_channel(two_gnutests_linked_p11):
    """The harmless neighbour of the two above: gnutest parts a channel of its
    own from inside a handler. The part is a nested channel event both modules
    see, and the dispatch it was called from is untouched."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub, channel_members=["other"])
    await command(hub, env["asker"], "gnutest", f"join {CHAN}")

    await command(hub, env["asker"], "gnutest", f"onevent Quit part {CHAN}")

    out = await drive(hub, env, [f"{env['victim']} Q :bye now"])
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest", "Quit victim bye now"),
        ("gnutest", f"ChannelPart {CHAN} gnutest -"),
        ("gnutest2", f"ChannelPart {CHAN} gnutest -"),
        ("gnutest2", "Quit victim bye now"),
    ]


@pytest.mark.asyncio
async def test_a_handler_that_unloads_itself(two_gnutests_linked_p11):
    """xServer::UnloadClient() is deferred to a timer, so the dispatch it was
    called from finishes untouched and the module goes away afterwards - which
    is itself an event, posted for the module's own client, and which the module
    being unloaded is still registered for."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub)

    await command(hub, env["asker"], "gnutest", "onevent Quit unloadself")

    # The timer may be reached before or after the message that ends the first
    # window, so both windows are read as one: what is pinned here is the order,
    # not which window the unload's own quit falls in
    start = len(hub.received)
    await drive(hub, env, [f"{env['victim']} Q :bye now"])
    alive(proc)

    # Nothing is logged for an unload; the module's client quitting is the sign
    gone = numnick(hub, "gnutest")
    await hub.wait_for(lambda line: line.startswith(f"{gone} Q "), timeout=30.0)

    await drive(hub, env, [f"{env['other']} Q :me too"])
    alive(proc)
    out = [strip_msg_tags(line) for line in hub.received[start:]]
    assert reports(hub, out, env) == [
        ("gnutest", "Quit victim bye now"),
        ("gnutest2", "Quit victim bye now"),
        ("gnutest", "Quit gnutest -"),
        ("gnutest2", "Quit gnutest -"),
        ("gnutest2", "Quit other me too"),
    ]


@pytest.mark.xfail(strict=False, reason=UB_ERASED)
@pytest.mark.asyncio
async def test_a_handler_that_detaches_itself(two_gnutests_linked_p11):
    """xServer::DetachClient() is not deferred: it deletes the xClient whose
    handler is running, and xServer::removeClient() takes that client out of
    every listener list - the one the dispatch is walking included - and posts
    the module's own quit from inside the event it is handling."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub)

    await command(hub, env["asker"], "gnutest", "onevent Quit detachself")

    out = await drive_or_die(hub, env, [f"{env['victim']} Q :bye now"], proc)
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest", "Quit victim bye now"),
        ("gnutest", "Quit gnutest -"),
        ("gnutest2", "Quit gnutest -"),
        ("gnutest2", "Quit victim bye now"),
    ]
