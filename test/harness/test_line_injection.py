"""Text must not be able to start a server-to-server line of its own.

ircu ends a line at CR as well as at LF. gnuworld splits what it receives at
LF only, so a CR can sit inside a message it is handed, and it used to write
that text out as given. Whatever followed the CR then reached the uplink as a
fresh line, from the services server: a SQUIT, a GLINE, anything.

An ordinary user cannot do this on a network of ircu servers, because their own
server splits their input at the CR first. It takes text from somewhere else:
a database field filled in through the web, a server that is not ircu, a
module bug. xServer::writeLine() now sends one line per call, whatever the
text.
"""

from __future__ import annotations

import time

import pytest

import gnutest_client as gt
from p10 import p10_token

CHAN = "#inj"
PAYLOAD = "Az SQ hub.testnet 0 :injected"


@pytest.mark.asyncio
async def test_a_carriage_return_inside_a_message_does_not_start_a_new_line(gnutest_linked_p11):
    hub, proc = gnutest_linked_p11
    asker = await hub.introduce_nick("asker", username="asker")
    alice = await hub.introduce_nick("alice", username="alice")
    ts = int(time.time()) - 3600
    await hub.send_raw(f"{hub.server_numnick} B {CHAN} {ts} +n {alice}:o")
    me = gt.numnick(hub)
    await gt.run(hub, asker, f"join {CHAN}")

    # gnutest's "say" repeats the text to the channel
    sent = await gt.run(hub, asker, f"say {CHAN} harmless\r{PAYLOAD}")

    assert sent == [f"{me} P {CHAN} :harmless"]
    assert all("\r" not in line and "injected" not in line for line in hub.received)
    assert not any(p10_token(line) == "SQ" for line in hub.received)

    # It said so in its log, and it is still linked and answering
    await proc.wait_for_stdout("bytes behind a line break", timeout=5.0)
    assert await gt.run(hub, asker, f"say {CHAN} still here") == [f"{me} P {CHAN} :still here"]
