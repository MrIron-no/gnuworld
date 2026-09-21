"""What a handler may do to the dispatch it is running inside.

Two mod.gnutest instances register for everything, gnutest first and gnutest2
second, so that every event reaches gnutest first: gnutest acts from inside its
handler ("onevent <NAME> <action>") and gnutest2 is the witness of what that
leaves of the dispatch. Each report names the module that sent it, so the order
below is the order gnuworld wrote them in.

The dispatch walks a copy of the listener list and counts its depth
(xServer::dispatch), so a handler may do what it likes to the registries and to
the network: what it unregisters, registers, kills or empties cannot end the
walk, and a network object core removes is held until the line being processed
is done with it, so every later handler is still handed something it can read -
and so is the core code that posted the event.

A post made from inside a handler is therefore not nested: it waits until the
event being dispatched has reached all of its subscribers, and the queue drains
breadth first. That is what most of these tests pin: every report below is in
the order gnuworld wrote it, and a module acting from inside a handler shows up
as a second round after the first, never in the middle of it.

Every test checks that gnuworld is still running afterwards, so an abort cannot
read as a pass.
"""

from __future__ import annotations

import asyncio
import time

import pytest

import gnutest_client as gt
from p10 import p10_token, strip_msg_tags

CHAN = "#order"

# The dispatch itself now survives a module detaching itself from inside a
# handler, and the order below is what it produces. What is still undefined is
# the module: xServer::removeClient() deletes the xClient whose handler is
# running and dlcloses its library under it, so the handler returns into freed
# memory. Nothing catches it today, which is why this is xfail non-strict and
# not a passing test. Deferring the unload past the dispatch is the next task.
UB_DETACHED = (
    "UB: DetachClient() deletes the running handler's own xClient and dlcloses "
    "its module; deferring the unload past the dispatch is the next task"
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


@pytest.mark.asyncio
async def test_a_handler_that_unregisters_itself(two_gnutests_linked_p11):
    """gnutest unregisters itself from inside the handler, which takes it out of
    the very list the dispatch came from. The dispatch walks a copy, so the event
    is still on its way to gnutest2, and from then on it reaches gnutest2
    alone."""
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
    """A post made from inside a handler is not dispatched there and then: it
    waits until the event being dispatched has reached every subscriber, and is
    delivered after it. The quit reason and the posted text are both a local of
    the caller's, which the queued post has to have copied."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub)

    await command(hub, env["asker"], "gnutest", "onevent Quit post")

    out = await drive(hub, env, [f"{env['victim']} Q :bye now"])
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest", "Quit victim bye now"),
        ("gnutest2", "Quit victim bye now"),
        ("gnutest", "Raw onevent post"),
        ("gnutest2", "Raw onevent post"),
    ]


@pytest.mark.asyncio
async def test_a_handler_that_kills_the_client_the_event_is_about(two_gnutests_linked_p11):
    """gnutest kills the joining client from inside the join, which takes the
    iClient and the ChannelUser that are the event's payload out of the network.
    Both are held until the line being processed is done with them, so gnutest2,
    next in the same dispatch, is handed two objects it can still read; the kill
    it is told about afterwards."""
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
        ("gnutest2", f"ChannelJoin {CHAN} victim victim"),
        ("gnutest", "Kill - victim onevent kill"),
        ("gnutest2", "Kill - victim onevent kill"),
    ]


@pytest.mark.asyncio
async def test_a_handler_that_empties_the_channel_the_event_is_about(two_gnutests_linked_p11):
    """gnutest kicks the only member of the channel the event is about, which
    takes the Channel out of the network (xServer::kickMembers parts the module it
    joined to kick with, and a channel left empty goes with it). It is held until
    the line being processed is done with it, so gnutest2, next in the same
    dispatch, is handed a Channel it can still read. The join and the part the kick
    needed are posted from inside the create, and so are reported after it.

    msg_C passes no ChannelUser with EVT_CREATE, which is why the member of the
    create is reported as "-"."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub)

    await command(hub, env["asker"], "gnutest", "onevent ChannelCreate kick")

    out = await drive_or_die(
        hub, env, [f"{env['victim']} C {CHAN} {int(time.time())}"], proc
    )
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest", f"ChannelCreate {CHAN} victim -"),
        ("gnutest2", f"ChannelCreate {CHAN} victim -"),
        ("gnutest", f"ChannelJoin {CHAN} gnutest gnutest"),
        ("gnutest2", f"ChannelJoin {CHAN} gnutest gnutest"),
        ("gnutest", f"ChannelPart {CHAN} gnutest -"),
        ("gnutest2", f"ChannelPart {CHAN} gnutest -"),
    ]


@pytest.mark.asyncio
async def test_a_kick_that_empties_the_channel_leaves_it_gone(two_gnutests_linked_p11):
    """The other half of the test above: the channel gnutest2 was handed is held
    for the line that removed it and no longer. A handler that wants to know
    whether what it is looking at is still on the network asks the network, and by
    the time the next line is read the channel is gone for good."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub)

    await command(hub, env["asker"], "gnutest", "onevent ChannelCreate kick")

    out = await drive_or_die(
        hub, env, [f"{env['victim']} C {CHAN} {int(time.time())}"], proc
    )
    alive(proc)
    assert ("gnutest2", f"ChannelCreate {CHAN} victim -") in reports(hub, out, env)

    # "chaninfo" is Network->findChannel(), as a handler would ask it
    out = await command(hub, env["asker"], "gnutest2", f"chaninfo {CHAN}")
    alive(proc)
    assert any("Unable to find channel" in line for line in out)


@pytest.mark.asyncio
async def test_a_wire_part_whose_handler_empties_the_channel(two_gnutests_linked_p11):
    """The caller side of the same thing, and the reason the holding list is not
    released where the dispatch ends: the post is the OUTERMOST dispatch, made by
    msg_L from a wire part, and msg_L reads the channel again after it
    ("if( theChan->empty() )") to remove a channel the part has emptied.

    victim and gnutest are the two members. victim parts, and gnutest's handler
    parts the channel from inside that event, which empties it and takes it out
    of the network - at depth 1, so the Channel is held. Releasing the holding
    list when the dispatch unwound freed it before msg_L looked at it again,
    which is a use-after-free in core's own hands and not in a module's: the
    channel is released once per main loop iteration instead, so the line that
    posted the event is done with it first.

    The channel the part emptied is gone by the time the next line is read, which
    is what the second half asserts: a longer lifetime, not a leak.

    mod.gnutest's "onevent <NAME> kick" cannot express the reviewer's version of
    this, which kicks the last other member: it kicks the client the event is
    about, and on a part that client has already left the channel, so
    xServer::kickMembers() finds nobody to kick and refuses. "part" empties the
    same channel from the same handler through the same held destroy(), and no
    mod.* file had to change to say it."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub, channel_members=["victim"])
    await command(hub, env["asker"], "gnutest", f"join {CHAN}")

    await command(hub, env["asker"], "gnutest", f"onevent ChannelPart part {CHAN}")

    out = await drive_or_die(hub, env, [f"{env['victim']} L {CHAN}"], proc)
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest", f"ChannelPart {CHAN} victim -"),
        ("gnutest2", f"ChannelPart {CHAN} victim -"),
        ("gnutest", f"ChannelPart {CHAN} gnutest -"),
        ("gnutest2", f"ChannelPart {CHAN} gnutest -"),
    ]

    # "chaninfo" is Network->findChannel(), as a handler would ask it
    out = await command(hub, env["asker"], "gnutest2", f"chaninfo {CHAN}")
    alive(proc)
    assert any("Unable to find channel" in line for line in out)


@pytest.mark.asyncio
async def test_a_handler_that_parts_a_channel(two_gnutests_linked_p11):
    """The harmless neighbour of the two above: gnutest parts a channel of its
    own from inside a handler. xServer::PartChannel() posts EVT_PART from there,
    so the part is a channel event both modules see after the quit they are in
    the middle of."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub, channel_members=["other"])
    await command(hub, env["asker"], "gnutest", f"join {CHAN}")

    await command(hub, env["asker"], "gnutest", f"onevent Quit part {CHAN}")

    out = await drive(hub, env, [f"{env['victim']} Q :bye now"])
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest", "Quit victim bye now"),
        ("gnutest2", "Quit victim bye now"),
        ("gnutest", f"ChannelPart {CHAN} gnutest -"),
        ("gnutest2", f"ChannelPart {CHAN} gnutest -"),
    ]


@pytest.mark.asyncio
async def test_a_chain_of_nested_posts_is_breadth_first(two_gnutests_linked_p11):
    """Three deep: the quit reaches both modules, gnutest's handler parts a
    channel, that part reaches both modules, gnutest2's handler posts an event of
    its own, and that reaches both modules. Each round is delivered whole before
    the round it posted, which is what the queue is for."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub, channel_members=["other"])
    await command(hub, env["asker"], "gnutest", f"join {CHAN}")

    await command(hub, env["asker"], "gnutest2", "onevent ChannelPart post")
    await command(hub, env["asker"], "gnutest", f"onevent Quit part {CHAN}")

    out = await drive(hub, env, [f"{env['victim']} Q :bye now"])
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest", "Quit victim bye now"),
        ("gnutest2", "Quit victim bye now"),
        ("gnutest", f"ChannelPart {CHAN} gnutest -"),
        ("gnutest2", f"ChannelPart {CHAN} gnutest -"),
        ("gnutest", "Raw onevent post"),
        ("gnutest2", "Raw onevent post"),
    ]


@pytest.mark.asyncio
async def test_a_handler_that_registers_the_event_being_dispatched(two_gnutests_linked_p11):
    """A registration made from inside a handler applies to later events, not to
    the one being dispatched. RegisterEvent() unregisters before it adds back, so
    this puts gnutest at the end of the very list this dispatch came from: it must
    not be called a second time for this event, and the reversed order of the next
    one is the proof that the registration did happen.

    gnutest can only register itself, so the listener added in the middle of the
    dispatch is its own; a listener added at the end of the list is exactly the
    one a positional walk would call again."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub)

    await command(hub, env["asker"], "gnutest", "onevent Quit register Quit")

    out = await drive(hub, env, [f"{env['victim']} Q :bye now"])
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest", "Quit victim bye now"),
        ("gnutest2", "Quit victim bye now"),
    ]

    out = await drive(hub, env, [f"{env['other']} Q :me too"])
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest2", "Quit other me too"),
        ("gnutest", "Quit other me too"),
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


@pytest.mark.xfail(strict=False, reason=UB_DETACHED)
@pytest.mark.asyncio
async def test_a_handler_that_detaches_itself(two_gnutests_linked_p11):
    """xServer::DetachClient() is not deferred: xServer::removeClient() posts the
    module's own quit, takes the module out of every listener list, and then
    deletes the xClient whose handler is running.

    The dispatch survives all three. The quit it was in the middle of still
    reaches gnutest2, and the module's own quit is delivered after it - by which
    time the module is no longer a listener, so it is not told of its own
    departure. That is the whole of what unregistering means here: the module asked
    to be detached, and a detached module receives nothing.

    Still xfail: the xClient is deleted and its library dlclosed while its own
    handler is on the stack, so the handler returns into freed memory. It happens
    to survive that, here and under the sanitizer both, which is luck and not a
    contract; deferring the unload past the dispatch is the next task."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub)

    await command(hub, env["asker"], "gnutest", "onevent Quit detachself")

    out = await drive_or_die(hub, env, [f"{env['victim']} Q :bye now"], proc)
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest", "Quit victim bye now"),
        ("gnutest2", "Quit victim bye now"),
        ("gnutest2", "Quit gnutest -"),
    ]
