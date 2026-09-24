"""A second end of burst from the uplink does not burst us all over again.

msg_EB's branch for our own uplink runs xServer::BurstClients() and
BurstChannels(), which is gnuworld's own burst: an N line for every local
client, and every module's BurstChannels() to join and claim what it wants.
That branch runs whenever the uplink says EB, and an uplink says it once, so
a second one is malformed, buggy or hostile - but it used to re-run both, and
the things a module arms from OnConnect()/BurstChannels() (timers that re-arm
themselves, among them) have nobody to cancel the first set.

So our own burst goes out at most once per uplink connection now. The rest of
that branch still runs: it is idempotent or already repeats for every other
server's EB, and gnuworld still answers the duplicate with EB and EA, which
is what test_events.py's test_the_uplinks_end_of_burst_acknowledges_itself
observes EVT_EA_SENT through.
"""

from __future__ import annotations

import pytest

import gnutest_client as gt
from p10 import p10_token, strip_msg_tags

OPERCHAN = "#gnutest-opers"


async def drive(hub, asker: str, lines: list[str], timeout: float = 10.0) -> list[str]:
    """Send ``lines`` to gnuworld and return everything it wrote because of them.

    Like gnutest_client.run(): gnuworld reads inbound lines in order and writes
    in order, so a one-word message to gnutest afterwards marks the end.
    """
    target = gt.numnick(hub)
    after = len(hub.received)
    for line in lines:
        await hub.send_raw(line)
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


@pytest.mark.asyncio
async def test_a_second_end_of_burst_does_not_burst_us_again(gnutest_linked_p11):
    """The handshake has already burst gnutest's client and its operchan. A
    second EB naming the uplink must not do either a second time: no further N
    line for a local client, and nothing more for the channel mod.gnutest joins
    from BurstChannels()."""
    hub, proc = gnutest_linked_p11
    asker = await hub.introduce_nick("asker", username="asker")

    out = await drive(hub, asker, [f"{hub.server_numnick} EB"])

    assert proc.proc is not None and proc.proc.returncode is None, "gnuworld died"

    # BurstClients() did not run again: no local client was introduced twice
    assert [line for line in out if p10_token(line) == "N"] == [], out

    # BurstChannels() did not run again: nothing more about gnutest's operchan
    assert [line for line in out if OPERCHAN in line] == [], out

    # The guard is around our own burst only: the duplicate is still
    # acknowledged, which is what EVT_EA_SENT is observed through
    assert f"{hub.peer_numeric} EB" in out and f"{hub.peer_numeric} EA" in out, out


@pytest.mark.asyncio
async def test_a_repeated_end_of_burst_is_logged(gnutest_linked_p11):
    """A protocol anomaly worth a line, not a crash - and the line stands in
    for the burst, so what the burst used to say is not said either: joining
    gnutest's operchan a second time is refused by xServer::JoinChannel(), and
    that refusal is the one trace BurstChannels() leaves of having re-run."""
    hub, proc = gnutest_linked_p11
    asker = await hub.introduce_nick("asker", username="asker")

    mark = len(proc.stdout_lines)
    await drive(hub, asker, [f"{hub.server_numnick} EB"])

    logged = await proc.wait_for_stdout("Repeated end of burst from our uplink", timeout=10.0)
    assert hub.name in logged, logged
    assert proc.proc is not None and proc.proc.returncode is None, "gnuworld died"

    since = proc.stdout_lines[mark:]
    assert [line for line in since if "more than once" in line] == [], since


@pytest.mark.asyncio
async def test_the_first_end_of_burst_still_bursts_us(gnutest_linked_p11):
    """The guard is invisible in the normal path: the handshake's own EB burst
    gnutest's client and joined its operchan, exactly once each."""
    hub, _proc = gnutest_linked_p11

    ours = [strip_msg_tags(line) for line in hub.received]
    assert [line for line in ours if p10_token(line) == "N" and " gnutest " in line] != []
    assert len([line for line in ours if p10_token(line) == "B" and OPERCHAN in line]) == 1
