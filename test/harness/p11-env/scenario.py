#!/usr/bin/env python3
"""Deterministic channel state for P11 burst captures.

Connects a handful of clients to the leaf of the capture stack (see run.sh)
and builds channel state that exercises every part of the P11 BURST layout.
The clients then stay connected. A burst is only sent at link time, so the
capture procedure is:

    ./run.sh up
    ./scenario.py &                 # build the state, keep clients online
    ./run.sh restart gnuworld       # gnuworld relinks and receives the burst
    cp capture/socket.log ../data/captures/<name>.log

What each channel is for:

  #p11-plain      ops, voices, op+voice and status-less members; a topic
  #p11-bans       three bans set by two different clients (P11 burst carries
                  "mask timestamp setter" per ban), plus +k and +l
  #p11-deljoin    +D with two hidden members, one member revealed by speaking
                  and one revealed by being voiced
  #p11-wasdel     was +D, then -D while a hidden member remained: the hidden
                  member must still be burst as ":d" on a channel without +D

Standard library only. Python 3.9+.
"""

import argparse
import asyncio
import sys


class Client:
    def __init__(self, nick: str, verbose: bool) -> None:
        self.nick = nick
        self.verbose = verbose
        self.reader: asyncio.StreamReader
        self.writer: asyncio.StreamWriter
        self._registered = asyncio.Event()
        self._task: asyncio.Task
        self._sync = 0
        self._pending: dict[str, asyncio.Future] = {}

    async def connect(self, host: str, port: int) -> None:
        self.reader, self.writer = await asyncio.open_connection(host, port)
        self.send(f"NICK {self.nick}")
        self.send(f"USER {self.nick.lower()} 0 * :p11 scenario {self.nick}")
        self._task = asyncio.create_task(self._pump())
        await asyncio.wait_for(self._registered.wait(), timeout=60)

    async def do(self, line: str) -> None:
        """Send a command and wait until the server has processed it.

        ircu throttles a client that sends many commands in a row, so a fixed
        sleep is not enough: commands from other clients overtake the queued
        ones. The server handles one client's commands in order, so the PONG
        for a PING sent right behind the command proves the command is done.
        """
        self._sync += 1
        token = f"sync{self._sync}"
        fut = asyncio.get_running_loop().create_future()
        self._pending[token] = fut
        self.send(line)
        self.send(f"PING :{token}")
        await self.writer.drain()
        await asyncio.wait_for(fut, timeout=120)

    def send(self, line: str) -> None:
        if self.verbose:
            print(f"{self.nick:>8} > {line}", flush=True)
        self.writer.write((line + "\r\n").encode())

    async def _pump(self) -> None:
        while True:
            raw = await self.reader.readline()
            if not raw:
                print(f"{self.nick}: connection closed by server", file=sys.stderr)
                return
            line = raw.decode(errors="replace").rstrip("\r\n")
            parts = line.split(" ")
            if parts[0] == "PING":
                self.send("PONG " + " ".join(parts[1:]))
            elif len(parts) > 1 and parts[1] == "PONG":
                fut = self._pending.pop(parts[-1].lstrip(":"), None)
                if fut and not fut.done():
                    fut.set_result(None)
            elif len(parts) > 1 and parts[1] in ("001",):
                self._registered.set()
            elif len(parts) > 1 and parts[1] in ("433", "432", "465", "464"):
                print(f"{self.nick}: registration refused: {line}", file=sys.stderr)
            elif self.verbose and len(parts) > 1 and parts[1][:1] in "45":
                # 4xx/5xx: an error numeric means the scenario did not apply
                print(f"{self.nick:>8} < {line}", flush=True)


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=16667, help="leaf client port (run.sh maps 16667)")
    ap.add_argument("--hold", type=float, default=0, help="seconds to stay online; 0 = until Ctrl-C")
    ap.add_argument("--late-reveal", type=float, default=0, metavar="SECONDS",
                    help="this long after the state is built, HidOne speaks in #p11-deljoin; "
                         "relink gnuworld in between to see the resulting P11 REVEAL (RV) live")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    nicks = ["OpOne", "OpTwo", "OpVoice", "VoiceOne", "PlainOne", "PlainTwo", "HidOne", "HidTwo", "HidThree"]
    c = {n: Client(n, args.verbose) for n in nicks}
    # Connect concurrently. With a debug-only gnuworld nothing answers the
    # leaf's iauth CHECK query, so every registration waits out iauthd's 15 s
    # timeout; done one by one that would add up to minutes.
    await asyncio.gather(*(cl.connect(args.host, args.port) for cl in c.values()))
    print(f"{len(c)} clients registered", flush=True)

    # Every step is confirmed by the server before the next one starts (see
    # Client.do), so the order below is the order the network sees.

    # --- #p11-plain -------------------------------------------------------
    await c["OpOne"].do("JOIN #p11-plain")
    for n in ("OpTwo", "OpVoice", "VoiceOne", "PlainOne", "PlainTwo"):
        await c[n].do("JOIN #p11-plain")
    await c["OpOne"].do("MODE #p11-plain +tn")
    await c["OpOne"].do("MODE #p11-plain +oov OpTwo OpVoice OpVoice")
    await c["OpOne"].do("MODE #p11-plain +v VoiceOne")
    await c["OpOne"].do("TOPIC #p11-plain :P11 capture scenario: plain channel")

    # --- #p11-bans --------------------------------------------------------
    await c["OpOne"].do("JOIN #p11-bans")
    await c["OpTwo"].do("JOIN #p11-bans")
    await c["OpOne"].do("MODE #p11-bans +o OpTwo")
    await c["OpOne"].do("MODE #p11-bans +tnkl sekrit 25")
    await c["OpOne"].do("MODE #p11-bans +b *!*@spam.example.net")
    await asyncio.sleep(1.1)  # distinct ban timestamps
    await c["OpTwo"].do("MODE #p11-bans +b *!*lamer@*.example.org")
    await asyncio.sleep(1.1)
    await c["OpTwo"].do("MODE #p11-bans +b nasty*!*@*")

    # --- #p11-deljoin -----------------------------------------------------
    await c["OpOne"].do("JOIN #p11-deljoin")
    await c["OpOne"].do("MODE #p11-deljoin +tnD")
    for n in ("HidOne", "HidTwo", "PlainOne", "VoiceOne"):
        await c[n].do("JOIN #p11-deljoin")  # all four join hidden
    await c["PlainOne"].do("PRIVMSG #p11-deljoin :speaking reveals me")
    await c["OpOne"].do("MODE #p11-deljoin +v VoiceOne")  # voicing reveals VoiceOne

    # --- #p11-wasdel ------------------------------------------------------
    await c["OpTwo"].do("JOIN #p11-wasdel")
    await c["OpTwo"].do("MODE #p11-wasdel +tnD")
    await c["HidThree"].do("JOIN #p11-wasdel")  # hidden
    await c["PlainTwo"].do("JOIN #p11-wasdel")  # hidden, then revealed below
    await c["PlainTwo"].do("PRIVMSG #p11-wasdel :speaking reveals me")
    await c["OpTwo"].do("MODE #p11-wasdel -D")  # HidThree stays hidden: local +d

    print("state built; clients staying online" + (f" for {args.hold:g}s" if args.hold else " (Ctrl-C to quit)"), flush=True)
    if args.late_reveal:
        await asyncio.sleep(args.late_reveal)
        await c["HidOne"].do("PRIVMSG #p11-deljoin :a late reveal, after gnuworld relinked")
        print("HidOne has spoken in #p11-deljoin", flush=True)

    try:
        if args.hold:
            await asyncio.sleep(args.hold)
        else:
            await asyncio.Event().wait()
    except asyncio.CancelledError:
        pass
    return 0


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except KeyboardInterrupt:
        sys.exit(0)
