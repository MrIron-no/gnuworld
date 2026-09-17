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
async def test_a_carriage_return_does_not_start_a_new_line(gnutest_linked_p11):
    hub, proc = gnutest_linked_p11
    asker = await hub.introduce_nick("asker", username="asker")
    alice = await hub.introduce_nick("alice", username="alice")
    ts = int(time.time()) - 3600
    await hub.send_raw(f"{hub.server_numnick} B {CHAN} {ts} +n {alice}:o")
    me = gt.numnick(hub)
    await gt.run(hub, asker, f"join {CHAN}")

    # A TOPIC goes straight to Write(), which sends one line and drops the rest
    sent = await gt.run(hub, asker, f"topic {CHAN} harmless\r{PAYLOAD}")
    assert len(sent) == 1 and sent[0].startswith(f"{me} T {CHAN} ") and sent[0].endswith(" :harmless")
    await proc.wait_for_stdout("bytes behind a line break", timeout=5.0)

    # A message is split at a line break instead, on purpose (see
    # test_messages.py). What followed the CR is then the text of a second
    # PRIVMSG: said to the channel, not executed by the uplink.
    sent = await gt.run(hub, asker, f"say {CHAN} harmless\r{PAYLOAD}")
    assert sent == [f"{me} P {CHAN} :harmless", f"{me} P {CHAN} :{PAYLOAD}"]

    # Either way, no line ever starts with the payload
    assert all("\r" not in line for line in hub.received)
    assert not any(p10_token(line) == "SQ" for line in hub.received)
    assert await gt.run(hub, asker, f"say {CHAN} still here") == [f"{me} P {CHAN} :still here"]
