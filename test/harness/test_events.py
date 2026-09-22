"""What core posts as an event, and what it passes with it.

Core posts each event through a named method of xClient with typed parameters
(``postQuit`` reaching ``OnQuit(iClient*, string_view)``, and so on), and
delivers it to a module that has not been converted yet as up to four ``void*``
that the module casts back by hand. These tests pin what arrives, from the
outside, for either API. mod.gnutest's ``events on`` registers for every network
event and for channel events on every channel, and reports each one it receives
as

    EVENT <NAME> <arg1> <arg2> ...

where every arg is the printable identity of that payload - a client's nick, a
server's name, a channel's name, a G-line's mask, or the string itself - and
"-" stands for a payload that is null or was not passed at all. A channel event
reports its channel first. The names come from one table in gnutest.cc, because
they are spelled without the spaces that eventNames[] in events.h has.

Each case names the wire line that produces the event. The expected list is
exact: an extra event, a missing one or two in the wrong order all fail - and so
does an event gnutest has no name for, which it reports as "?". EVT_RAW is the
one event left out of that comparison (core posts it for every line the uplink
sends, so it would appear in every case); test_raw_is_every_line_read covers it
on its own.

Three of the events in events.h are posted nowhere at all: EVT_JUPE and
EVT_UNJUPE (a jupe is posted as a net join and its removal as nothing) and
EVT_KICK (a kick goes to xClient::OnNetworkKick() instead).
"""

from __future__ import annotations

import time

import pytest

import gnutest_client as gt
from p10 import int_to_b64, ipv4_to_b64, p10_token, server_numeric, strip_msg_tags

CHAN = "#events"
LEAF = "leaf.testnet"
LEAF_NUMERIC = 5


def alive(proc) -> None:
    assert proc.proc is not None and proc.proc.returncode is None, "gnuworld died"


async def setup(hub) -> dict[str, str]:
    """Everything the cases refer to, introduced before reporting is turned on
    so that only what a case drives is reported."""
    env = {
        "asker": await hub.introduce_nick("asker", username="asker"),
        "victim": await hub.introduce_nick("victim", username="victim"),
        "other": await hub.introduce_nick("other", username="other"),
        "hub": hub.server_numnick,
        "us": hub.peer_numeric,
        "gnutest": gt.numnick(hub),
        "leafyy": server_numeric(LEAF_NUMERIC),
        "ts": str(int(time.time())),
        "ip": ipv4_to_b64("127.0.0.1"),
    }
    env["leafmask"] = env["leafyy"] + int_to_b64(262143, 3)
    env["leafuser"] = env["leafyy"] + "AAB"
    return env


def reports(lines: list[str], env: dict[str, str], *, raw: bool = False) -> list[str]:
    """The ``EVENT ...`` notices gnutest sent to the asker, in order.

    Only a NOTICE from gnutest's own numeric counts, so nothing core writes
    itself can make one of these assertions pass.
    """
    out = []
    marker = f" {env['asker']} :EVENT "
    for line in lines:
        if p10_token(line) != "O" or marker not in line:
            continue
        if not line.startswith(f"{env['gnutest']} O "):
            continue
        report = line.split(marker, 1)[1]
        if not raw and report.split(" ")[0] == "Raw":
            continue
        out.append(report)
    return out


async def drive(hub, env: dict[str, str], lines: list[str], timeout: float = 10.0) -> list[str]:
    """Send ``lines`` to gnuworld and return everything it wrote because of them.

    Like gnutest_client.run(): gnuworld reads inbound lines in order and writes
    in order, so a one-word message to gnutest afterwards marks the end, and
    everything before gnutest's fixed answer to it belongs to ``lines``.
    """
    asker = env["asker"]
    after = len(hub.received)
    for line in lines:
        await hub.send_raw(line)
    await hub.send_privmsg(asker, env["gnutest"], "sync")

    def is_sentinel(line: str) -> bool:
        return p10_token(line) == "O" and f" {asker} :" in line and gt.SENTINEL_REPLY in line

    await hub.wait_for(is_sentinel, timeout=timeout, after=after)

    out = []
    for line in hub.received[after:]:
        if is_sentinel(line):
            break
        out.append(strip_msg_tags(line))
    return out


def leaf(env: dict[str, str]) -> list[str]:
    """A leaf server behind the hub, with one client on it."""
    return [
        f"{env['hub']} S {LEAF} 2 0 {env['ts']} J10 {env['leafmask']} + :Test leaf",
        f"{env['leafyy']} N leafuser 2 {env['ts']} leafuser {LEAF} +i "
        f"{env['ip']} {env['leafuser']} :Leaf User",
    ]


def channel(env: dict[str, str]) -> list[str]:
    """CHAN, with victim opped and other in it."""
    return [f"{env['hub']} B {CHAN} {env['ts']} +tn {env['victim']}:o,{env['other']}"]


def command(env: dict[str, str], text: str) -> str:
    """A chat command to gnutest, which calls the core API for it."""
    return f"{env['asker']} P {env['gnutest']} :{text}"


# (name, lines sent before reporting is on, lines that drive the event,
#  the reports expected for them, in order)
CASES = [
    # <victim> M victim :+o - a user mode change to +o (msg_M)
    (
        "oper",
        lambda e: [],
        lambda e: [f"{e['victim']} M victim :+o"],
        lambda e: ["OperUp victim"],
    ),
    # <hub> N <nick> ... - a client introduced by the hub (msg_N)
    (
        "nick",
        lambda e: [],
        lambda e: [
            f"{e['hub']} N newguy 1 {e['ts']} newguy fake.testnet +i {e['ip']} ABAAZ :New Guy"
        ],
        lambda e: ["ClientConnect newguy"],
    ),
    # <victim> N <newnick> <ts> - a nick change (msg_N, xNetwork::rehashNick)
    (
        "chnick",
        lambda e: [],
        lambda e: [f"{e['victim']} N renamed {e['ts']}"],
        lambda e: ["NickChange renamed victim"],
    ),
    # <victim> Q :<reason> - a client quit, the one path that passes a reason
    (
        "quit",
        lambda e: [],
        lambda e: [f"{e['victim']} Q :bye now"],
        lambda e: ["Quit victim bye now"],
    ),
    # <hub> SQ <leaf> <ts> :<reason> - a squit: the server breaks, and every
    # client on it quits with it (xNetwork::removeServer). The second payload of
    # a net break is the server the broken one was linked to, which msg_SQ
    # resolves from the numeric on the object; it used to be the prefix of the
    # SQ line, which nothing could do anything with
    (
        "netbreak_and_split_quit",
        leaf,
        lambda e: [f"{e['hub']} SQ {LEAF} {e['ts']} :hub says so"],
        lambda e: [f"NetBreak {LEAF} hub.testnet hub says so", "Quit leafuser Server split"],
    ),
    # <hub> D <victim> :<reason> - a kill by a server (msg_D)
    (
        "kill_by_server",
        lambda e: [],
        lambda e: [f"{e['hub']} D {e['victim']} :hub.testnet!op (go away)"],
        lambda e: ["Kill hub.testnet victim hub.testnet!op (go away)"],
    ),
    # <other> D <victim> :<reason> - the same kill from a client
    (
        "kill_by_client",
        lambda e: [],
        lambda e: [f"{e['other']} D {e['victim']} :other (go away)"],
        lambda e: ["Kill other victim other (go away)"],
    ),
    # "kill <nick> <reason>" - xClient::Kill(), which passes no source at all
    (
        "kill_by_module",
        lambda e: [],
        lambda e: [command(e, "kill victim go away")],
        lambda e: ["Kill - victim go away"],
    ),
    # <hub> GL * +<mask> <expire> <lastmod> :<reason> (msg_GL)
    (
        "gline",
        lambda e: [],
        lambda e: [f"{e['hub']} GL * +*@bad.example 3600 {e['ts']} :spam"],
        lambda e: ["GlineAdd *@bad.example"],
    ),
    # <hub> GL * -<mask> - the removal of that G-line
    (
        "remgline",
        lambda e: [f"{e['hub']} GL * +*@bad.example 3600 {e['ts']} :spam"],
        lambda e: [f"{e['hub']} GL * -*@bad.example"],
        lambda e: ["GlineRemove *@bad.example"],
    ),
    # <hub> S <name> ... - a server joins the network (msg_S), the one post
    # site of the three that passes the new server's uplink
    (
        "netjoin",
        lambda e: [],
        lambda e: [leaf(e)[0]],
        lambda e: [f"NetJoin {LEAF} hub.testnet"],
    ),
    # <hub> JU * +<name> <expire> <ts> :<reason> - a jupe, which is posted as a
    # net join (msg_JU has a TODO for EVT_JUPE): no ServerJupe is ever reported
    (
        "jupe_is_a_netjoin",
        lambda e: [],
        lambda e: [f"{e['hub']} JU * +juped.testnet 604800 {e['ts']} :juped"],
        lambda e: ["NetJoin juped.testnet -"],
    ),
    # <hub> JU * -<name> ... - and its removal posts nothing at all: msg_JU's
    # removal branch has no post, so no EVT_UNJUPE and no net break are ever
    # reported for one. That the removal did happen is what the squit after it
    # shows: for a server that is still there it would report a net break, as
    # the netbreak case above does.
    (
        "unjupe_posts_nothing",
        lambda e: [f"{e['hub']} JU * +juped.testnet 604800 {e['ts']} :juped"],
        lambda e: [
            f"{e['hub']} JU * -juped.testnet 604800 {e['ts']} :unjuped",
            f"{e['hub']} SQ juped.testnet {e['ts']} :gone",
        ],
        lambda e: [],
    ),
    # "spawnserver <name> <description>" - xServer::AttachServer()
    (
        "netjoin_of_a_fake_server",
        lambda e: [],
        lambda e: [command(e, "spawnserver fake.testnet moo")],
        lambda e: ["NetJoin fake.testnet -"],
    ),
    # <leaf> EB - another server has finished its burst (msg_EB)
    (
        "burst_complete",
        leaf,
        lambda e: [f"{e['leafyy']} EB"],
        lambda e: [f"BurstComplete {LEAF}"],
    ),
    # <leaf> EA - an end of burst acknowledgement (msg_EA)
    (
        "burst_ack",
        leaf,
        lambda e: [f"{e['leafyy']} EA"],
        lambda e: [f"BurstAcknowledge {LEAF}"],
    ),
    # <hub> XQ <us> <routing> :<message> (msg_XQ, xServer::OnXQuery): routing
    # and message are a const char*, not a std::string*, unlike every other
    # string payload
    (
        "xquery",
        lambda e: [],
        lambda e: [f"{e['hub']} XQ {e['us']} tok :query text"],
        lambda e: ["XQuery hub.testnet tok query text"],
    ),
    # <hub> XR <us> <routing> :<reply> (msg_XR, xServer::OnXReply)
    (
        "xreply",
        lambda e: [],
        lambda e: [f"{e['hub']} XR {e['us']} tok :reply text"],
        lambda e: ["XReply hub.testnet tok reply text"],
    ),
    # <hub> CF <ts> <key> :<value> - a netconf variable is set (msg_CF)
    (
        "netconf",
        lambda e: [],
        lambda e: [f"{e['hub']} CF {e['ts']} mykey :myvalue"],
        lambda e: ["NetconfAdd hub.testnet mykey"],
    ),
    # <hub> CF <ts> <key> - the same line with no value removes it
    (
        "remnetconf",
        lambda e: [f"{e['hub']} CF {e['ts']} mykey :myvalue"],
        lambda e: [f"{e['hub']} CF {int(e['ts']) + 1} mykey"],
        lambda e: ["NetconfRemove hub.testnet mykey"],
    ),
    # <hub> AC <victim> <account> - a login (msg_AC)
    (
        "account",
        lambda e: [],
        lambda e: [f"{e['hub']} AC {e['victim']} testacct 42 0"],
        lambda e: ["AccountLogin victim"],
    ),
    # the same for a client that is already +r: a change of flags
    (
        "account_flags",
        lambda e: [f"{e['victim']} M victim :+r"],
        lambda e: [f"{e['hub']} AC {e['victim']} testacct 42 2"],
        lambda e: ["AccountFlags victim"],
    ),
    # "spawnclient <nick>" - a fake client of a module (xServer::AttachClient)
    (
        "nick_of_a_fake_client",
        lambda e: [],
        lambda e: [command(e, "spawnclient fakeguy")],
        lambda e: ["ClientConnect fakeguy"],
    ),
    # "removeclient <nick>" - and its quit (xServer::DetachClient), whose
    # reason is the one it writes to the network
    (
        "quit_of_a_fake_client",
        lambda e: [command(e, "spawnclient fakeguy")],
        lambda e: [command(e, "removeclient fakeguy")],
        lambda e: ["Quit fakeguy Requested shutdown"],
    ),
    # <victim> C <chan> <ts> - a client creates a channel (msg_C). A create
    # names the creator and nothing else: xClient::OnCreate() takes no
    # ChannelUser, so all three of its post sites report "-" for one
    (
        "channel_create",
        lambda e: [],
        lambda e: [f"{e['victim']} C {CHAN} {e['ts']}"],
        lambda e: [f"ChannelCreate {CHAN} victim -"],
    ),
    # <victim> J <chan> <ts> for a channel that does not exist yet is a create,
    # not a join (msg_J)
    (
        "channel_create_by_join",
        lambda e: [],
        lambda e: [f"{e['victim']} J {CHAN} {e['ts']}"],
        lambda e: [f"ChannelCreate {CHAN} victim -"],
    ),
    # "join <chan>" for a channel that does not exist: one of our own modules
    # creates it (xServer::JoinChannel)
    (
        "channel_create_of_our_own_client",
        lambda e: [],
        lambda e: [command(e, f"join {CHAN}")],
        lambda e: [f"ChannelCreate {CHAN} gnutest -"],
    ),
    # <other> J <chan> <ts> - a join to a channel that exists (msg_J)
    (
        "channel_join",
        lambda e: [f"{e['hub']} B {CHAN} {e['ts']} +tn {e['victim']}:o"],
        lambda e: [f"{e['other']} J {CHAN} {e['ts']}"],
        lambda e: [f"ChannelJoin {CHAN} other other"],
    ),
    # "join <chan>" - one of our own modules joins (xServer::JoinChannel)
    (
        "channel_join_of_our_own_client",
        lambda e: channel(e),
        lambda e: [command(e, f"join {CHAN}")],
        lambda e: [f"ChannelJoin {CHAN} gnutest gnutest"],
    ),
    # <hub> B <chan> <ts> +tn <members> - a channel in a net burst (msg_B)
    (
        "channel_burst",
        lambda e: [],
        lambda e: [f"{e['hub']} B {CHAN} {e['ts']} +tn {e['victim']}:o,{e['other']}"],
        lambda e: [f"ChannelBurst {CHAN} victim victim", f"ChannelBurst {CHAN} other other"],
    ),
    # <victim> L <chan> :<message> - a part, the one path with a part message
    (
        "channel_part",
        channel,
        lambda e: [f"{e['victim']} L {CHAN} :gone"],
        lambda e: [f"ChannelPart {CHAN} victim gone"],
    ),
    # <victim> J 0 - a client parts every channel it is on (msg_J), with no
    # part message to pass
    (
        "channel_part_of_all_channels",
        channel,
        lambda e: [f"{e['victim']} J 0"],
        lambda e: [f"ChannelPart {CHAN} victim -"],
    ),
    # <victim> T <chan> :<topic> (msg_T)
    (
        "channel_topic",
        channel,
        lambda e: [f"{e['victim']} T {CHAN} :new topic"],
        lambda e: [f"ChannelTopicChange {CHAN} victim new topic"],
    ),
    # <hub> T <chan> :<topic> - a topic from a server, as one in a burst is:
    # there is no client to pass, so the payload is null
    (
        "channel_topic_from_a_server",
        channel,
        lambda e: [f"{e['hub']} T {CHAN} :server topic"],
        lambda e: [f"ChannelTopicChange {CHAN} - server topic"],
    ),
    # "spawnjoin <nick> <chan>" - a module's fake client joins
    # (xServer::JoinChannel for an iClient)
    (
        "channel_join_of_a_fake_client",
        lambda e: channel(e) + [command(e, "spawnclient fakeguy")],
        lambda e: [command(e, f"spawnjoin fakeguy {CHAN}")],
        lambda e: [f"ChannelJoin {CHAN} fakeguy fakeguy"],
    ),
    # "spawnpart <nick> <chan>" - and its part (xServer::PartChannel), with no
    # part message passed
    (
        "channel_part_of_a_fake_client",
        lambda e: channel(e)
        + [command(e, "spawnclient fakeguy"), command(e, f"spawnjoin fakeguy {CHAN}")],
        lambda e: [command(e, f"spawnpart fakeguy {CHAN}")],
        lambda e: [f"ChannelPart {CHAN} fakeguy -"],
    ),
    # "kick <chan> <nick> <reason>" - a kick by one of our own modules posts no
    # event about the victim at all (xServer::kickMembers takes the member off
    # the channel itself and reports the kick through OnNetworkKick): what the
    # events say is that the module joined the channel to kick from it and then
    # parted again, the part with no message (xServer::OnPartChannel)
    (
        "kick_by_our_own_client_is_a_join_and_a_part_of_the_module",
        channel,
        lambda e: [command(e, f"kick {CHAN} victim get out")],
        lambda e: [f"ChannelJoin {CHAN} gnutest gnutest", f"ChannelPart {CHAN} gnutest -"],
    ),
    # <hub> M <chan> +m - a mode change by a server (msg_M): the one channel
    # event whose payload is an iServer and not an iClient
    (
        "channel_servermode",
        channel,
        lambda e: [f"{e['hub']} M {CHAN} +m"],
        lambda e: [f"ChannelModeByServer {CHAN} hub.testnet"],
    ),
    # <hub> CM <chan> <modes> - clearmode by a server (msg_CM), the same event
    (
        "channel_servermode_by_clearmode",
        channel,
        lambda e: [f"{e['hub']} CM {CHAN} b"],
        lambda e: [f"ChannelModeByServer {CHAN} hub.testnet"],
    ),
]


@pytest.mark.parametrize(
    "prepare,driven,expected",
    [case[1:] for case in CASES],
    ids=[case[0] for case in CASES],
)
@pytest.mark.asyncio
async def test_core_posts_this_event(gnutest_linked_p11, prepare, driven, expected):
    hub, proc = gnutest_linked_p11
    env = await setup(hub)

    for line in prepare(env):
        await hub.send_raw(line)
    # gnuworld reads in order, so the command that turns reporting on is also
    # what makes sure everything above has been handled
    await gt.run(hub, env["asker"], "events on")

    out = await drive(hub, env, driven(env))
    alive(proc)
    assert reports(out, env) == expected(env)


@pytest.mark.asyncio
async def test_nothing_is_reported_until_events_are_on(gnutest_linked_p11):
    """The control case: gnutest is registered for nothing until it is asked,
    so the same lines that every case above drives produce no report. Without
    this, a case that matched something core writes itself would pass."""
    hub, proc = gnutest_linked_p11
    env = await setup(hub)

    for prepare, driven, _expected in [case[1:] for case in CASES]:
        for line in prepare(env) + driven(env):
            await hub.send_raw(line)
    out = await drive(hub, env, [])
    alive(proc)
    assert reports(out, env, raw=True) == []


@pytest.mark.asyncio
async def test_a_kick_from_the_network_posts_no_channel_event(gnutest_linked_p11):
    """EVT_KICK is never posted anywhere, and a kick is not a part either: core
    delivers a kick to the modules through xClient::OnNetworkKick(), which is
    not an event and which gnutest does not report. The kick was handled all the
    same - for a client of ours core confirms it with an L line (msg_K)."""
    hub, proc = gnutest_linked_p11
    env = await setup(hub)

    for line in channel(env):
        await hub.send_raw(line)
    await gt.run(hub, env["asker"], f"join {CHAN}")
    await gt.run(hub, env["asker"], "events on")

    out = await drive(hub, env, [f"{env['hub']} K {CHAN} {env['gnutest']} :kicked"])
    alive(proc)
    assert [line for line in out if p10_token(line) == "L"] == [f"{env['gnutest']} L {CHAN}"]
    assert reports(out, env) == []


@pytest.mark.asyncio
async def test_raw_is_every_line_read(gnutest_linked_p11):
    """EVT_RAW carries the line itself, and core posts it for every line it
    reads, after the line has been handled."""
    hub, proc = gnutest_linked_p11
    env = await setup(hub)
    await gt.run(hub, env["asker"], "events on")

    # Core posts EVT_RAW after the line has been handled, so the report for the
    # message that ends a command arrives after the answer to it: read that one
    # before measuring, or it turns up in the window below
    await hub.drain_messages(0.3)

    # A notice to gnutest is handled by no event of its own, so Raw is all
    notice = f"{env['hub']} O {env['gnutest']} :hello there"
    out = await drive(hub, env, [notice])
    alive(proc)
    assert reports(out, env, raw=True) == [f"Raw {notice}"]


@pytest.mark.asyncio
async def test_the_uplinks_end_of_burst_acknowledges_itself(gnutest_linked_p11):
    """EVT_EA_SENT is posted at one place only: the branch of msg_EB for our own
    uplink, right after gnuworld has written its EB and EA. That branch runs
    whenever the uplink says EB, so a second one after the link is up is the
    only way to see the event at all - during the handshake itself nothing can
    be registered yet."""
    hub, proc = gnutest_linked_p11
    env = await setup(hub)
    await gt.run(hub, env["asker"], "events on")

    out = await drive(hub, env, [f"{env['hub']} EB"])
    alive(proc)
    assert reports(out, env) == ["BurstComplete hub.testnet", "BurstAcknowledgeSent hub.testnet"]


@pytest.mark.parametrize(
    "prefix",
    [
        pytest.param(lambda e: e["leafyy"] + int_to_b64(1, 3), id="unknown_numnick"),
        pytest.param(lambda e: e["leafyy"] + "A", id="three_characters"),
        pytest.param(lambda e: e["leafyy"], id="unknown_server_numeric"),
    ],
)
@pytest.mark.asyncio
async def test_an_xquery_from_a_source_we_cannot_resolve_is_refused(gnutest_linked_p11, prefix):
    """msg_XQ has to name the server the query came from, because that is what
    it hands to the modules, and a prefix of three characters or more is a
    client whose server it looks up. Nothing says the prefix is one we know: an
    XQ from a numnick of no client of ours, or a source of any length that is
    neither a client nor a server, must be refused rather than crash the daemon
    or reach a module with no server at all.

    The leaf of the other cases is never introduced here, so none of these
    prefixes names anything gnuworld has heard of."""
    hub, proc = gnutest_linked_p11
    env = await setup(hub)
    await gt.run(hub, env["asker"], "events on")

    out = await drive(hub, env, [f"{prefix(env)} XQ {env['us']} tok :from nowhere"])
    alive(proc)
    assert reports(out, env) == []
