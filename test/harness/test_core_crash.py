"""Security repro: malformed S2S lines from the uplink into xServer::Process.

Two core parsing defects:

1. xServer::Process (src/server.cc) copies the first whitespace-delimited
   token of a non-':'-prefixed line into a fixed 10-byte stack buffer
   `char YXX[10]` with NO bounds check:

       while (*s && (' ' != *s)) { *yxxPtr++ = *s++; }

   A source/first token longer than 9 bytes overflows the stack.

2. xServer::OnRead (src/server_connection.cc) computes
       size_t len = line.size() - 1;
       while ('\n' == line[len] || '\r' == line[len]) --len;
   which underflows to SIZE_MAX for a line made only of CR/LF bytes
   (e.g. "\r\n"), reading out of bounds.

The FakeHub is gnuworld's uplink; after handshake it sends raw lines.
"""

from __future__ import annotations

import asyncio

import pytest


async def _alive(proc) -> bool:
    return proc.proc is not None and proc.proc.returncode is None


@pytest.mark.asyncio
async def test_yxx_stack_overflow(linked):
    """Oversized first token overflows char YXX[10] -> crash/abort."""
    hub, proc = linked

    # Sanity: gnuworld is up and processing before the attack.
    await hub.drain_messages(timeout=0.5)
    assert await _alive(proc), "gnuworld not alive before attack"

    # First token (the 'source numeric' slot) is 4000 bytes with no space.
    # Legitimate P10 sources are 2 or 5 chars; this smashes YXX[10].
    payload = "A" * 4000 + " EB"
    await hub.send_raw(payload)

    # Give it a moment to process and (if vulnerable) die.
    await asyncio.sleep(2.0)

    # A healthy server keeps answering; probe with a PING-like round trip
    # by sending a normal line and checking the process is still alive.
    alive = await _alive(proc)
    rc = proc.proc.returncode if proc.proc else None
    recent = "\n".join(proc.stdout_lines[-15:])
    assert alive, (
        f"gnuworld terminated (returncode={rc}) after oversized first "
        f"token -> YXX[10] stack overflow.\nRecent output:\n{recent}"
    )


@pytest.mark.asyncio
async def test_empty_crlf_line_underflow(linked):
    """A bare CR/LF-only line underflows len in OnRead (OOB read)."""
    hub, proc = linked

    await hub.drain_messages(timeout=0.5)
    assert await _alive(proc), "gnuworld not alive before attack"

    # Send several CR/LF-only 'lines'. Buffer::ReadLine strips leading '\n'
    # then splits on '\n'; "\r\n" yields a line "\r\n" of pure CR/LF.
    for _ in range(50):
        await hub.send_raw("\r")  # send_raw appends "\r\n" -> line is "\r\r\n"

    await asyncio.sleep(2.0)

    alive = await _alive(proc)
    rc = proc.proc.returncode if proc.proc else None
    recent = "\n".join(proc.stdout_lines[-15:])
    assert alive, (
        f"gnuworld terminated (returncode={rc}) after CR/LF-only lines "
        f"-> OnRead len underflow.\nRecent output:\n{recent}"
    )
