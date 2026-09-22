"""Adversarial attacks on the five guarantees of src/server_events.cc, from the
wire: that every subscriber sees every event in order; that a post made from a
handler is queued, not dispatched re-entrantly; that an object lives until the
line delivering it is done, even if a handler destroys it; that the network
tables a handler asks mid-dispatch already reflect what just happened; and that
a module which unloads itself from inside a handler goes only once no frame of
it is left on the stack.

test_event_order.py already pins these guarantees for the case where the
*first*-registered subscriber (gnutest) is the one that acts and the second
(gnutest2) is a passive witness, and for a single object destroyed once. These
tests go where that file does not: the *last*-registered subscriber acting
instead of the first, two subscribers posting from the very same dispatch, a
squit - whose split delivers a NetBreak and then a Quit through two separate
top-level dispatches - with a handler nested inside each, and a quit that
empties a channel as a side effect nobody asked it to report.

The last four go further still, to what an armed action could not name until
mod.gnutest let it: one instance unloading *another* from inside a handler, a
handler killing a client and emptying a channel that the event it is running
inside has nothing to do with, and a chain of five events each posted from the
handler of the one before it.

Everything reuses two_gnutests_linked_p11 and the drive()/reports()/command()
machinery of test_event_order.py, which spells out the wire protocol these
tests speak. A comment above each case says which guarantee it is aimed at.
"""

from __future__ import annotations

import asyncio
import time

import pytest

import gnutest_client as gt
from p10 import p10_token, strip_msg_tags
from test_event_order import CHAN, alive, command, drive, drive_or_die, logged, numnick, reports, setup

LEAF = "split.testnet"
LEAF_NUM = 42

# The channels the five-deep chain walks, one per hop
CHAIN = ["#chain-a", "#chain-b", "#chain-c"]


async def leaf_setup(hub) -> dict[str, str]:
    """asker, and a leaf server with one client behind it, everything in place
    before reporting is turned on so that only what a test drives is reported
    (mirrors test_event_order.setup(), for the squit tests that need a server
    to break rather than a channel)."""
    env = {"asker": await hub.introduce_nick("asker", username="asker")}
    leaf_yy = await hub.introduce_leaf(LEAF, LEAF_NUM, protocol="J11")
    env["leafuser"] = await hub.introduce_leaf_nick(leaf_yy, LEAF_NUM, "leafuser")
    await command(hub, env["asker"], "gnutest", "events on")
    await command(hub, env["asker"], "gnutest2", "events on")
    return env


async def drive_to(hub, env, lines, target, proc, timeout: float = 10.0) -> list[str]:
    """test_event_order.drive(), but the message marking the end of the window
    goes to ``target`` instead of always gnutest2: drive() itself depends on
    gnutest2 outliving the window it measures, which does not hold in the one
    test below where gnutest2 is what leaves."""
    asker = env["asker"]
    after = len(hub.received)
    for line in lines:
        await hub.send_raw(line)
    await hub.send_privmsg(asker, numnick(hub, target), "sync")

    def is_sentinel(line: str) -> bool:
        return p10_token(line) == "O" and f" {asker} :" in line and gt.SENTINEL_REPLY in line

    try:
        await hub.wait_for(is_sentinel, timeout=timeout, after=after)
    except (ConnectionError, OSError, TimeoutError):
        await asyncio.sleep(1.0)
        alive(proc)
        raise

    out = []
    for line in hub.received[after:]:
        if is_sentinel(line):
            break
        out.append(strip_msg_tags(line))
    return out


@pytest.mark.asyncio
async def test_the_last_registered_module_empties_the_channel_the_first_only_witnesses(
    two_gnutests_linked_p11,
):
    """Guarantee 1 (delivery order) attacked from the other end: every existing
    test that destroys something has gnutest, the *first*-registered
    subscriber, do the destroying. Here gnutest2 - registered second, called
    second, and so already the last frame on the stack for this event - is the
    one that joins, kicks and parts. If delivery order ever depended on which
    module *acted* rather than on registration order, gnutest would either miss
    the aftermath or see it out of turn. It does not: gnutest, despite having
    already returned from the ChannelCreate call before gnutest2 does anything,
    still gets every one of the three events gnutest2's kick chains into, and
    still gets them first, in every round."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub)

    await command(hub, env["asker"], "gnutest2", "onevent ChannelCreate kick")

    out = await drive_or_die(hub, env, [f"{env['victim']} C {CHAN} {int(time.time())}"], proc)
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest", f"ChannelCreate {CHAN} victim victim"),
        ("gnutest2", f"ChannelCreate {CHAN} victim victim"),
        ("gnutest", f"ChannelJoin {CHAN} gnutest2 gnutest2"),
        ("gnutest2", f"ChannelJoin {CHAN} gnutest2 gnutest2"),
        ("gnutest", f"ChannelKick {CHAN} gnutest2 victim onevent kick zombie"),
        ("gnutest2", f"ChannelKick {CHAN} gnutest2 victim onevent kick zombie"),
        ("gnutest", f"ChannelPart {CHAN} gnutest2 -"),
        ("gnutest2", f"ChannelPart {CHAN} gnutest2 -"),
    ]


@pytest.mark.asyncio
async def test_two_handlers_that_both_post_from_the_same_event_get_separate_rounds(
    two_gnutests_linked_p11,
):
    """Guarantee 2, attacked with two originators instead of one: gnutest and
    gnutest2 both act on the very same Quit, each posting a Raw event from
    inside their own OnQuit. Both posts are made while dispatchDepth is 1, so
    both must be queued rather than dispatched there and then.

    A correct, non-reentrant queue delivers Quit to both, only then the first
    queued Raw to both, only then the second queued Raw to both: six reports,
    (gnutest, gnutest2) paired three times over. A reentrant implementation -
    one where gnutest's post recursed into dispatch from inside its own OnQuit
    - would instead interleave a Raw pair between the two Quit reports, since
    gnutest2 would not yet have been called for the Quit it is still waiting
    on: (Quit, Raw, Raw, Quit, Raw, Raw) is what that bug would print, and is
    distinguishable from the correct order by position alone even though every
    Raw report has identical text."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub)

    await command(hub, env["asker"], "gnutest", "onevent Quit post")
    await command(hub, env["asker"], "gnutest2", "onevent Quit post")

    out = await drive(hub, env, [f"{env['victim']} Q :bye now"])
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest", "Quit victim bye now"),
        ("gnutest2", "Quit victim bye now"),
        ("gnutest", "Raw onevent post"),
        ("gnutest2", "Raw onevent post"),
        ("gnutest", "Raw onevent post"),
        ("gnutest2", "Raw onevent post"),
    ]


@pytest.mark.asyncio
async def test_a_squits_nested_post_drains_before_the_split_quits_are_posted(
    two_gnutests_linked_p11,
):
    """Guarantee 2, attacked through a real multi-phase core operation instead
    of mod.gnutest's own "part"/"post" chain: msg_SQ posts NetBreak, and only
    once that whole dispatch - including anything it queued - has settled does
    it call xNetwork::OnSplit(), which posts a separate, later Quit for every
    client the broken server carried. That is two independent calls into
    notify(), not one nested dispatch, so it is the one place in this codebase
    where "the queue drains before the *next line* is read" (as the docstring
    of destroy() puts it) is exercised by two Post*() calls neither of which
    nests inside the other.

    gnutest posts a Raw from inside NetBreak. If dispatchDepth were not back to
    zero by the time OnSplit's postQuit runs - if, say, releasing it were
    forgotten between the two phases of a squit - the split's Quit would queue
    behind the nested Raw instead of running as its own top-level dispatch, or
    the nested Raw would leak into the wrong round. Instead every report below
    is in the order gnuworld wrote it: NetBreak, the nested Raw fully drained,
    and only then the leaf's Quit - never interleaved, and never the other way
    round."""
    hub, proc = two_gnutests_linked_p11
    env = await leaf_setup(hub)

    await command(hub, env["asker"], "gnutest", "onevent NetBreak post")

    out = await drive_or_die(hub, env, [f"{hub.server_numnick} SQ {LEAF} 0 :bye"], proc)
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest", f"NetBreak {LEAF} hub.testnet bye"),
        ("gnutest2", f"NetBreak {LEAF} hub.testnet bye"),
        ("gnutest", "Raw onevent post"),
        ("gnutest2", "Raw onevent post"),
        ("gnutest", "Quit leafuser Server split"),
        ("gnutest2", "Quit leafuser Server split"),
    ]


@pytest.mark.asyncio
async def test_a_split_quits_handler_cannot_kill_a_client_the_network_already_forgot(
    two_gnutests_linked_p11,
):
    """Guarantees 3 and 4 pulled apart by a squit, where they disagree in a way
    no other wire line produces. msg_Q.cc posts an ordinary Quit *before*
    removing the client, precisely so a handler still finds it in the network
    (see its own comment, and test_event_order's kill-race tests, which rely
    on that to make their second Kill() meaningful). xNetwork::removeServer()
    does the opposite for a split: it erases the client from every table
    *first*, and only then calls postQuit() - so a handler of this Quit has an
    iClient it can still read (nothing has deleted it yet; that waits for the
    destroy() a few lines below in Network.cc), but Network->stillHas() has
    been false since before the very first listener was called, not just after
    some earlier handler acted.

    gnutest is armed to Kill() the client the Quit is about. xClient::Kill()
    is the one piece of module-facing API that asks the network mid-handler
    (see src/client.cc), and it must refuse exactly as it does when a second
    handler chases a first handler's kill: no EVT_KILL is posted, no D goes
    out, and nothing logs a client numeric it could not find - because Kill()
    never gets far enough to ask xNetwork::removeClient() to remove it a
    second time."""
    hub, proc = two_gnutests_linked_p11
    env = await leaf_setup(hub)

    await command(hub, env["asker"], "gnutest", "onevent Quit kill")

    out = await drive_or_die(hub, env, [f"{hub.server_numnick} SQ {LEAF} 0 :bye"], proc)
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest", f"NetBreak {LEAF} hub.testnet bye"),
        ("gnutest2", f"NetBreak {LEAF} hub.testnet bye"),
        ("gnutest", "Quit leafuser Server split"),
        ("gnutest2", "Quit leafuser Server split"),
    ]
    assert [line for line in out if p10_token(line) == "D"] == []
    assert logged(proc, "Unable to find client numeric") == []


@pytest.mark.asyncio
async def test_the_last_registered_module_detaches_itself_the_first_reports_its_departure(
    two_gnutests_linked_p11,
):
    """Guarantee 5, attacked from the other registration slot: every existing
    self-detach/self-unload test has gnutest - first in every listener list -
    ask to go. Here gnutest2, last in the list and so the final frame called
    for this Quit, detaches itself instead. Nothing about xServer::removeClient
    (which unregisters immediately, and defers only the deletion and the
    unmap) reads a module's position in the list, so this must come out
    exactly as the mirror image of the existing test: gnutest, still very much
    a listener, is the one - and the only one - told about gnutest2's own
    quit, once gnutest2 is no longer on the list to be told itself."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub)

    await command(hub, env["asker"], "gnutest2", "onevent Quit detachself")

    # gnutest2 is what leaves, so the window has to end on a message to
    # gnutest instead of test_event_order.drive()'s usual gnutest2; and, as in
    # test_event_order's own self-detach test, the deferred departure may
    # land after that window ends rather than inside it, so it is waited for
    # separately and the two windows are read as one
    start = len(hub.received)
    await drive_to(hub, env, [f"{env['victim']} Q :bye now"], "gnutest", proc, timeout=30.0)
    alive(proc)
    await hub.wait_for(lambda line: "EVENT Quit gnutest2 " in line, timeout=30.0)

    out = [strip_msg_tags(line) for line in hub.received[start:]]
    assert reports(hub, out, env) == [
        ("gnutest", "Quit victim bye now"),
        ("gnutest2", "Quit victim bye now"),
        ("gnutest", "Quit gnutest2 -"),
    ]


@pytest.mark.asyncio
async def test_a_quit_that_empties_a_channel_reports_no_part(two_gnutests_linked_p11):
    """Not a race, but the other half of "events about a channel that is
    emptied": xNetwork::removeClient(), the path a plain quit takes, drops the
    ChannelUser and - if that leaves it empty - the Channel itself with no
    postPart() at all (unlike msg_L and msg_J's "part all channels", which both
    call postPart() explicitly). A subscriber is not told a channel it is
    watching went away because its last member quit; it only ever finds out
    the same way core itself finds out, by asking. Pinned here so that a
    future change which starts posting one is a deliberate one: gnutest is on
    no channel of its own, victim is the channel's only member, and after
    victim's quit the channel is confirmed gone by asking the network for it,
    the same way test_a_kick_that_empties_the_channel_leaves_it_gone does."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub, channel_members=["victim"])

    out = await drive_or_die(hub, env, [f"{env['victim']} Q :bye now"], proc)
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest", "Quit victim bye now"),
        ("gnutest2", "Quit victim bye now"),
    ]

    # "chaninfo" is Network->findChannel(), as a handler would ask it
    out = await command(hub, env["asker"], "gnutest2", f"chaninfo {CHAN}")
    alive(proc)
    assert any("Unable to find channel" in line for line in out)


@pytest.mark.asyncio
async def test_one_instance_detaches_the_other_from_inside_a_handler(two_gnutests_linked_p11):
    """Guarantee 5 with the asking and the going pulled apart: every other
    detach test has a module ask for itself, so xServer::DetachClient() has only
    ever been called on the handler's own frame. Here gnutest, in the middle of
    its own OnQuit, detaches gnutest2 - a module with no frame on the stack at
    all, and the one listener still to be called for this very event.

    DetachClient(xClient*) unregisters immediately, so gnutest2 is off the
    listener list before the dispatch reaches it: it is not told about the quit
    it was queued to receive, and the deferred half of the removal posts its own
    quit afterwards, which gnutest - still a listener - reports."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub)

    await command(hub, env["asker"], "gnutest", "onevent Quit detach gnutest2")

    # gnutest2 is what leaves, so both windows end on a message to gnutest, and
    # are read as one: the departure is deferred past the line that asked for it
    start = len(hub.received)
    await drive_to(hub, env, [f"{env['victim']} Q :bye now"], "gnutest", proc, timeout=30.0)
    alive(proc)
    await hub.wait_for(lambda line: "EVENT Quit gnutest2 " in line, timeout=30.0)

    out = [strip_msg_tags(line) for line in hub.received[start:]]
    assert reports(hub, out, env) == [
        ("gnutest", "Quit victim bye now"),
        ("gnutest", "Quit gnutest2 -"),
    ]


@pytest.mark.asyncio
async def test_one_instance_unloads_the_other_from_inside_a_handler(two_gnutests_linked_p11):
    """The same attack through UnloadClient() instead of DetachClient(), which
    is the pair's other half and the one a module is told to use.

    gnutest, from inside its own OnQuit, asks for gnutest2 to be unloaded. What
    leaves is gnutest2: its client quits, and gnutest - which asked - is still
    there afterwards to report the next event.

    The detach test above is the control, and this one used to be the pair's
    odd one out: UnloadClient(xClient*) looked the module up, threw the object
    away, kept only the module NAME and handed that to a timer, and
    DetachClient(const string&) then took the first entry of clientModuleList
    with that name. Both instances here come from libgnutest.la and answer to
    one name, so the unload landed on gnutest, the instance nobody named. What
    waits out the timer now is the instance itself."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub)

    await command(hub, env["asker"], "gnutest", "onevent Quit unload gnutest2")

    named, asked = numnick(hub, "gnutest2"), numnick(hub, "gnutest")
    start = len(hub.received)
    await hub.send_raw(f"{env['victim']} Q :bye now")

    # Whichever instance goes, its client quits: waiting for either says which
    # one it was without waiting out a timeout to find out
    await hub.wait_for(
        lambda line: p10_token(strip_msg_tags(line)) == "Q"
        and strip_msg_tags(line).split(" ", 1)[0] in (named, asked),
        timeout=30.0,
    )
    alive(proc)

    out = [strip_msg_tags(line) for line in hub.received[start:]]
    assert [line.split(" ", 1)[0] for line in out if p10_token(line) == "Q"] == [named], (
        "the unload took an instance the handler did not name"
    )

    # And the instance that asked is still a listener
    out = await drive_to(hub, env, [f"{env['other']} Q :me too"], "gnutest", proc, timeout=30.0)
    alive(proc)
    out = [strip_msg_tags(line) for line in hub.received[start:]]
    assert reports(hub, out, env) == [
        ("gnutest", "Quit victim bye now"),
        ("gnutest2", "Quit victim bye now"),
        ("gnutest", "Quit gnutest2 -"),
        ("gnutest", "Quit other me too"),
    ]


@pytest.mark.asyncio
async def test_a_handler_kills_a_client_the_event_was_never_about(two_gnutests_linked_p11):
    """Guarantee 3 for an object the dispatch never handed anyone. Every
    existing kill test has the handler kill the client the event is about, which
    is the client core itself is holding for the line; the holding list and
    xClient::Kill()'s "is it still there" guard are not written for that case
    alone.

    gnutest kills other - a client nothing in this dispatch mentions - from
    inside victim's Quit. The kill is a removal made at dispatchDepth 1 just
    like any other, so the EVT_KILL it posts is queued behind the quit it
    interrupted rather than dispatched re-entrantly, both modules are told about
    it in the round after, and exactly one D goes out for the client that was
    named."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub)

    await command(hub, env["asker"], "gnutest", "onevent Quit kill other")

    out = await drive_or_die(hub, env, [f"{env['victim']} Q :bye now"], proc)
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest", "Quit victim bye now"),
        ("gnutest2", "Quit victim bye now"),
        ("gnutest", "Kill - other onevent kill"),
        ("gnutest2", "Kill - other onevent kill"),
    ]

    killed = [line for line in out if p10_token(line) == "D"]
    assert len(killed) == 1, killed
    assert env["other"] in killed[0]
    assert logged(proc, "Unable to find client numeric") == []


@pytest.mark.asyncio
async def test_a_handler_empties_a_channel_the_event_was_never_about(two_gnutests_linked_p11):
    """The same again for a Channel, which is the object destroy()'s holding
    list was written for: test_event_order's kick tests empty the channel the
    event is about, so the Channel that goes is the one the caller of notify()
    is still holding a pointer to. Here the Quit being dispatched is about a
    client on no channel at all, and the channel gnutest empties belongs to
    nothing in the line being processed - it is reached only because the handler
    named it.

    It must still come apart in the usual order: the join and the part
    xServer::kickMembers() needs are posted from inside the quit and so arrive
    in the round after it, the kick is not an event and reaches both modules as
    it happens, and the emptied channel is gone by the time anything asks the
    network for it again."""
    hub, proc = two_gnutests_linked_p11
    # other is CHAN's only member, and is not what the quit is about
    env = await setup(hub, channel_members=["other"])

    await command(hub, env["asker"], "gnutest", f"onevent Quit kick {CHAN} other")

    out = await drive_or_die(hub, env, [f"{env['victim']} Q :bye now"], proc)
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest", "Quit victim bye now"),
        ("gnutest2", "Quit victim bye now"),
        ("gnutest", f"ChannelJoin {CHAN} gnutest gnutest"),
        ("gnutest2", f"ChannelJoin {CHAN} gnutest gnutest"),
        ("gnutest", f"ChannelKick {CHAN} gnutest other onevent kick zombie"),
        ("gnutest2", f"ChannelKick {CHAN} gnutest other onevent kick zombie"),
        ("gnutest", f"ChannelPart {CHAN} gnutest -"),
        ("gnutest2", f"ChannelPart {CHAN} gnutest -"),
    ]

    # "chaninfo" is Network->findChannel(), as a handler would ask it
    out = await command(hub, env["asker"], "gnutest2", f"chaninfo {CHAN}")
    alive(proc)
    assert any("Unable to find channel" in line for line in out)


@pytest.mark.asyncio
async def test_a_chain_of_five_nested_posts_is_breadth_first(two_gnutests_linked_p11):
    """Guarantee 2 driven as deep as the queue will go. Three was the most one
    action per instance could reach; five hops, each posted from the handler of
    the one before it, is where a queue that is really breadth first and one
    that merely looks it stop agreeing - a re-entrant dispatch would nest five
    frames deep and print gnutest's whole chain before gnutest2 was told of the
    quit it is still waiting on.

    gnutest arms one action per hop: the quit parts the first channel, that part
    parts the second, that part parts the third, and the last posts a Raw.
    Every round is delivered to both modules before the round it posted, ten
    reports in five pairs, and gnuworld is still running at the end - a chain
    this long is also the deepest the module can push whatever recursion core
    does or does not have."""
    hub, proc = two_gnutests_linked_p11
    env = await setup(hub, channel_members=["other"])
    for chan in CHAIN:
        await command(hub, env["asker"], "gnutest", f"join {chan}")

    await command(hub, env["asker"], "gnutest", f"onevent Quit part {CHAIN[0]}")
    await command(hub, env["asker"], "gnutest", f"onevent ChannelPart part {CHAIN[1]}")
    await command(hub, env["asker"], "gnutest", f"onevent ChannelPart part {CHAIN[2]}")
    await command(hub, env["asker"], "gnutest", "onevent ChannelPart post")

    out = await drive_or_die(hub, env, [f"{env['victim']} Q :bye now"], proc)
    alive(proc)
    assert reports(hub, out, env) == [
        ("gnutest", "Quit victim bye now"),
        ("gnutest2", "Quit victim bye now"),
        ("gnutest", f"ChannelPart {CHAIN[0]} gnutest -"),
        ("gnutest2", f"ChannelPart {CHAIN[0]} gnutest -"),
        ("gnutest", f"ChannelPart {CHAIN[1]} gnutest -"),
        ("gnutest2", f"ChannelPart {CHAIN[1]} gnutest -"),
        ("gnutest", f"ChannelPart {CHAIN[2]} gnutest -"),
        ("gnutest2", f"ChannelPart {CHAIN[2]} gnutest -"),
        ("gnutest", "Raw onevent post"),
        ("gnutest2", "Raw onevent post"),
    ]
