"""The logging system: logging.conf, SIGHUP, the built-in default, the
channel sink, and per-module SQL loggers.

Written from specs/2026-09-18-logger-core.md, not from the implementation.
Cases needing Postgres skip the way the rest of the harness does when Docker
is unavailable (docker_stack.up() raises, or the module is not in this
build). Cases needing the local build tree's own command-line flags (no -D,
an explicit -f/-d, SIGHUP) skip under GNUWORLD_HARNESS=docker, which has no
equivalent for them (see conftest.LocalGnuworldProc's docstring) and whose
conf-dir bind mount is read-only in any case.
"""

from __future__ import annotations

import asyncio
import json
import re
import signal
import subprocess
from contextlib import asynccontextmanager
from pathlib import Path

import pytest

import cservice_client as cs
import gnutest_client as gt
from conftest import (
    LocalGnuworldProc,
    _prepare_conf_dir,
    drop_connection,
    link_bare,
    link_cservice_logging,
    link_ccontrol_logging,
    override_conf_keys,
    require_module,
    send_gnuworld_signal,
)
from gnuworld_proc import CONTAINER_UPLINK, COMPOSE_FILE, HARNESS_DIR, GnuworldProc, use_docker
from p10 import p10_token, strip_msg_tags

TEXT_LINE = re.compile(
    r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}  "
    r"(FATAL|ERROR|WARN |INFO |DEBUG|TRACE)  \S+\s+.+"
)

# The same on the console, which shows the time of day and not the date.  It
# tells a log record from the raw protocol dump of -L, whose lines are what
# went over the wire and are nothing the logging system decided to show.
CONSOLE_LINE = re.compile(
    r"^\d{2}:\d{2}:\d{2}\.\d{3}  (FATAL|ERROR|WARN |INFO |DEBUG|TRACE)  \S+\s+.+"
)


def _notice_text(line: str, channel: str) -> str | None:
    """The text of a server NOTICE (P10 token "O") sent to ``channel``, or
    None when ``line`` is not that."""
    raw = strip_msg_tags(line)
    if p10_token(raw) != "O":
        return None
    parts = raw.split(" ", 3)
    if len(parts) < 4 or parts[2] != channel:
        return None
    return parts[3][1:] if parts[3].startswith(":") else parts[3]


async def _wait_for_notice(hub, channel: str, timeout: float = 10.0, after: int = 0,
                           contains: str = "") -> str:
    """The next NOTICE to ``channel`` whose text contains ``contains``.  A test
    names the record it is about: other records may reach the channel first
    (a SIGHUP, for one, is itself logged before the reload it causes)."""
    def pred(line: str) -> bool:
        text = _notice_text(line, channel)
        return text is not None and contains in text

    line = await hub.wait_for(pred, timeout=timeout, after=after)
    return _notice_text(line, channel)


def _require_local(reason: str = "writes into the conf dir, which the docker-mode mount is read-only for") -> None:
    if use_docker():
        pytest.skip(reason)


def _read_lines(path: Path) -> list[str]:
    return [l for l in path.read_text(encoding="utf-8", errors="replace").splitlines() if l.strip()]


def _read_json_lines(path: Path) -> list[dict]:
    return [json.loads(l) for l in _read_lines(path)]


# --------------------------------------------------------------------------
# 1. No logging.conf: debug.log, text layout, appends across restarts.
# --------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_no_logging_conf_writes_text_debug_log_and_appends(fake_hub, tmp_path):
    _require_local()
    hub = fake_hub
    conf_dir = _prepare_conf_dir(tmp_path)
    GnuworldProc.write_config(
        conf_dir / "GNUWorld.conf", local=True, uplink=CONTAINER_UPLINK, port=hub.port,
        password=hub.password,
    )

    proc = LocalGnuworldProc(conf_dir, extra_args=["-c", "-d", "debug.log"])
    await proc.start()
    try:
        await hub.accept_and_handshake(timeout=45.0)
        await proc.wait_for_stdout("Connected", timeout=30.0)
        await proc.wait_for_stdout("No logging.conf found", timeout=10.0)
    finally:
        await proc.terminate()

    debug_log = conf_dir / "debug.log"
    assert debug_log.is_file()
    lines = _read_lines(debug_log)
    assert lines, "debug.log is empty"

    for line in lines:
        assert TEXT_LINE.match(line), f"line does not match the text layout: {line!r}"

    assert any("No logging.conf found; using built-in defaults" in l for l in lines)
    # Core itself logs through the logger: a bare start, with no module that
    # still writes to elog, leaves no "legacy" line, but it does say on
    # "core.net" that it connected
    assert any(re.search(r"  core\.net\s+Connected to ", l) for l in lines), \
        "no 'core.net' connection line in debug.log"

    first_line = lines[0]
    first_count = len(lines)

    # A second start appends: the first line is untouched, and the file grows
    await drop_connection(hub)
    proc2 = LocalGnuworldProc(conf_dir, extra_args=["-c", "-d", "debug.log"])
    await proc2.start()
    try:
        await hub.accept_and_handshake(timeout=45.0)
        await proc2.wait_for_stdout("Connected", timeout=30.0)
        await proc2.wait_for_stdout("No logging.conf found", timeout=10.0)
    finally:
        await proc2.terminate()

    lines2 = _read_lines(debug_log)
    assert len(lines2) > first_count
    assert lines2[0] == first_line
    assert lines2[:first_count] == lines


# --------------------------------------------------------------------------
# 2. The harness's normal -D: no debug.log at all.
# --------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_harness_default_no_debug_log(linked, tmp_path):
    hub, proc = linked
    conf_dir = tmp_path / "etc-gnuworld"
    assert not (conf_dir / "debug.log").exists()


# --------------------------------------------------------------------------
# 3. logging.conf with a JSON file sink on root.
# --------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_json_file_sink_on_root(docker_stack, fake_hub, tmp_path):
    _require_local()
    hub = fake_hub
    root = GnuworldProc.conf_root(tmp_path / "etc-gnuworld")
    log_path = f"{root}/main.log"
    logging_conf = (
        "sink.mainlog.type = file\n"
        f"sink.mainlog.path = {log_path}\n"
        "sink.mainlog.format = json\n"
        "logger.root = INFO, mainlog\n"
        "logger.legacy = DEBUG\n"
    )

    async with link_bare(docker_stack, hub, tmp_path, logging_conf=logging_conf) as (
        hub, proc, conf_dir,
    ):
        await proc.wait_for_stdout("Connected", timeout=30.0)
        await asyncio.sleep(0.3)  # let a few startup records land

    records = _read_json_lines(Path(log_path))
    assert records, "the JSON sink never received a record"

    for record in records:
        assert {"timestamp", "level", "logger", "message"}.issubset(record.keys())
        assert isinstance(record["message"], str)

    assert not any("No logging.conf found" in r["message"] for r in records)


@pytest.mark.asyncio
async def test_logging_example_conf_parses_and_gnuworld_links(docker_stack, fake_hub, tmp_path):
    """bin/logging.example.conf, copied verbatim: gnuworld links, gnuworld.log
    is valid JSON lines, debug.log is text."""
    _require_local()
    hub = fake_hub
    conf_dir = _prepare_conf_dir(tmp_path)
    repo_root = Path(__file__).resolve().parents[2]
    example = (repo_root / "bin" / "logging.example.conf").read_text(encoding="utf-8")
    (conf_dir / "logging.conf").write_text(example, encoding="utf-8")

    GnuworldProc.write_config(
        conf_dir / "GNUWorld.conf", local=True, uplink=CONTAINER_UPLINK, port=hub.port,
        password=hub.password,
    )

    # The example file's sinks use bare relative paths ("debug.log",
    # "gnuworld.log"): gnuworld's cwd is the conf dir in local mode, so they
    # land right there with no rewriting needed.
    proc = LocalGnuworldProc(conf_dir, extra_args=["-c"])
    await proc.start()
    try:
        await hub.accept_and_handshake(timeout=45.0)
        await proc.wait_for_stdout("Connected", timeout=30.0)
        await asyncio.sleep(0.3)
    finally:
        await proc.terminate()

    assert not any("No logging.conf found" in l for l in proc.stdout_lines)
    assert not any("logging.conf:" in l for l in proc.stdout_lines), proc.stdout_lines

    gnuworld_log = conf_dir / "gnuworld.log"
    assert gnuworld_log.is_file()
    records = _read_json_lines(gnuworld_log)
    assert records
    for record in records:
        assert {"timestamp", "level", "logger", "message"}.issubset(record.keys())

    debug_log = conf_dir / "debug.log"
    assert debug_log.is_file()
    for line in _read_lines(debug_log):
        assert TEXT_LINE.match(line), f"line does not match the text layout: {line!r}"


# --------------------------------------------------------------------------
# 5. SIGHUP: rotation, and a broken file that leaves routing unchanged.
# --------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_sighup_rotates_the_json_file(docker_stack, fake_hub, tmp_path):
    _require_local()
    hub = fake_hub
    root = GnuworldProc.conf_root(tmp_path / "etc-gnuworld")
    log_path = Path(f"{root}/main.log")
    logging_conf = (
        "sink.mainlog.type = file\n"
        f"sink.mainlog.path = {log_path}\n"
        "sink.mainlog.format = json\n"
        "logger.root = INFO, mainlog\n"
    )

    async with link_bare(docker_stack, hub, tmp_path, logging_conf=logging_conf) as (
        hub, proc, conf_dir,
    ):
        await proc.wait_for_stdout("Connected", timeout=30.0)
        await asyncio.sleep(0.2)
        assert log_path.is_file()

        moved = log_path.with_name("main.log.1")
        log_path.rename(moved)
        assert not log_path.exists()

        await send_gnuworld_signal(proc, signal.SIGHUP)

        # A new file appears at the original path and says it was reloaded
        deadline = asyncio.get_event_loop().time() + 15.0
        while not log_path.exists() and asyncio.get_event_loop().time() < deadline:
            await asyncio.sleep(0.1)
        assert log_path.is_file(), "no new file appeared at the rotated path after SIGHUP"

        records = _read_json_lines(log_path)
        assert any("Reloaded" in r["message"] and "logging.conf" in r["message"] for r in records)


@pytest.mark.asyncio
async def test_sighup_with_a_broken_logging_conf_keeps_routing(docker_stack, fake_hub, tmp_path):
    _require_local()
    hub = fake_hub
    conf_dir = _prepare_conf_dir(tmp_path)
    root = GnuworldProc.conf_root(conf_dir)
    log_path = Path(f"{root}/main.log")
    conf_path = conf_dir / "logging.conf"

    good = (
        "sink.mainlog.type = file\n"
        f"sink.mainlog.path = {log_path}\n"
        "sink.mainlog.format = json\n"
        "logger.root = INFO, mainlog\n"
    )
    conf_path.write_text(good, encoding="utf-8")

    GnuworldProc.write_config(
        conf_dir / "GNUWorld.conf", local=True, uplink=CONTAINER_UPLINK, port=hub.port,
        password=hub.password,
    )
    with (conf_dir / "GNUWorld.conf").open("a", encoding="utf-8") as fh:
        fh.write(f"\nlogging_conf = {root}/logging.conf\n")

    proc = LocalGnuworldProc(conf_dir, extra_args=["-c"])
    await proc.start()
    try:
        await hub.accept_and_handshake(timeout=45.0)
        await proc.wait_for_stdout("Connected", timeout=30.0)
        await asyncio.sleep(0.2)

        # Break the file: an unparsable level. A whole new file, not an
        # appended line - a second "logger.root" line would itself be a
        # (different) parse error, "duplicate logger line", which is not
        # the case this is about.
        broken = (
            "sink.mainlog.type = file\n"
            f"sink.mainlog.path = {log_path}\n"
            "sink.mainlog.format = json\n"
            "logger.root = NOPE\n"
        )
        conf_path.write_text(broken, encoding="utf-8")
        before = len(_read_json_lines(log_path))

        await send_gnuworld_signal(proc, signal.SIGHUP)

        # The process is alive, and still routing to the same file
        await asyncio.sleep(1.0)
        assert proc.proc.returncode is None

        deadline = asyncio.get_event_loop().time() + 10.0
        error_seen = False
        while asyncio.get_event_loop().time() < deadline:
            records = _read_json_lines(log_path)
            error_seen = any(
                r["logger"] == "core.config" and r["level"] == "ERROR"
                and r["message"].startswith("logging.conf:") and "logger.root" in r["message"]
                for r in records
            )
            if error_seen:
                break
            await asyncio.sleep(0.2)
        assert error_seen, "no ERROR record on core.config naming logger.root"

        # Routing still works: a further record lands in the same file
        after = len(_read_json_lines(log_path))
        assert after >= before

        # Repair the file and reload again: a fresh "Reloaded" line appears
        conf_path.write_text(good, encoding="utf-8")
        await send_gnuworld_signal(proc, signal.SIGHUP)

        deadline = asyncio.get_event_loop().time() + 10.0
        reloaded = False
        while asyncio.get_event_loop().time() < deadline:
            records = _read_json_lines(log_path)
            reloaded = any("Reloaded" in r["message"] for r in records)
            if reloaded:
                break
            await asyncio.sleep(0.2)
        assert reloaded, "no 'Reloaded' record after the file was repaired"
    finally:
        await proc.terminate()


# --------------------------------------------------------------------------
# 7. A logging.conf whose file sink cannot open at start-up.
# --------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_unopenable_sink_falls_back_to_the_built_in_default(docker_stack, fake_hub, tmp_path):
    _require_local()
    hub = fake_hub
    conf_dir = _prepare_conf_dir(tmp_path)
    logging_conf = (
        "sink.mainlog.type = file\n"
        "sink.mainlog.path = /no/such/directory/at/all/main.log\n"
        "logger.root = INFO, mainlog\n"
    )
    (conf_dir / "logging.conf").write_text(logging_conf, encoding="utf-8")

    GnuworldProc.write_config(
        conf_dir / "GNUWorld.conf", local=True, uplink=CONTAINER_UPLINK, port=hub.port,
        password=hub.password,
    )

    proc = LocalGnuworldProc(conf_dir, extra_args=["-c"])
    await proc.start()
    try:
        await hub.accept_and_handshake(timeout=45.0)
        await proc.wait_for_stdout("Connected", timeout=30.0)
        await proc.wait_for_stdout("logging.conf:", timeout=15.0)
    finally:
        await proc.terminate()

    assert any("logging.conf:" in l for l in proc.stdout_lines)


# --------------------------------------------------------------------------
# 8. Level routing: OFF silences a logger, a sub-logger override is honoured.
# --------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_level_routing_off_and_sub_logger_override(docker_stack, fake_hub, tmp_path):
    _require_local()
    hub = fake_hub
    root = GnuworldProc.conf_root(tmp_path / "etc-gnuworld")
    log_path = f"{root}/main.log"
    cc_log_path = f"{root}/core-config.log"
    conf_dir = _prepare_conf_dir(tmp_path)
    conf_path = conf_dir / "logging.conf"

    # "additivity.core.config = no" plus a sink of its own is the observable
    # sub-logger override: an ERROR that core.config logs must land in
    # core-config.log and NOT in main.log, which only the override achieves
    # (without it, core.config's records would walk up to root's sink too).
    good = (
        "sink.mainlog.type = file\n"
        f"sink.mainlog.path = {log_path}\n"
        "sink.mainlog.format = json\n"
        "sink.cclog.type = file\n"
        f"sink.cclog.path = {cc_log_path}\n"
        "sink.cclog.format = json\n"
        "logger.root = INFO, mainlog\n"
        "logger.legacy = OFF\n"
        "logger.core.config = ERROR, cclog\n"
        "additivity.core.config = no\n"
    )
    conf_path.write_text(good, encoding="utf-8")

    GnuworldProc.write_config(
        conf_dir / "GNUWorld.conf", local=True, uplink=CONTAINER_UPLINK, port=hub.port,
        password=hub.password,
    )
    with (conf_dir / "GNUWorld.conf").open("a", encoding="utf-8") as fh:
        fh.write(f"\nlogging_conf = {root}/logging.conf\n")

    proc = LocalGnuworldProc(conf_dir, extra_args=["-c"])
    await proc.start()
    try:
        await hub.accept_and_handshake(timeout=45.0)
        await proc.wait_for_stdout("Connected", timeout=30.0)
        await asyncio.sleep(0.3)

        # A first, still-good SIGHUP: "core" gets its "Reloaded ..." INFO
        # record, so main.log is not simply empty (nothing else logs on
        # "core" during a quiet, module-less run)
        await send_gnuworld_signal(proc, signal.SIGHUP)
        await asyncio.sleep(0.5)

        # A whole new, broken file (not an appended line: see the SIGHUP test
        # above) and a second reload, so core.config gets an ERROR record
        broken = (
            "sink.mainlog.type = file\n"
            f"sink.mainlog.path = {log_path}\n"
            "sink.mainlog.format = json\n"
            "sink.cclog.type = file\n"
            f"sink.cclog.path = {cc_log_path}\n"
            "sink.cclog.format = json\n"
            "logger.root = NOPE\n"
        )
        conf_path.write_text(broken, encoding="utf-8")
        await send_gnuworld_signal(proc, signal.SIGHUP)
        await asyncio.sleep(1.0)
    finally:
        await proc.terminate()

    main_records = _read_json_lines(Path(log_path))
    cc_records = _read_json_lines(Path(cc_log_path))

    assert main_records
    assert not any(r["logger"] == "legacy" for r in main_records), "legacy = OFF did not silence it"
    assert any(r["logger"] == "core" for r in main_records), "core records still arrive"

    assert any(
        r["logger"] == "core.config" and r["level"] == "ERROR" for r in cc_records
    ), "the core.config sub-logger's own sink never got its ERROR record"
    assert not any(
        r["logger"] == "core.config" for r in main_records
    ), "additivity.core.config = no did not stop the walk to root's sink"


# --------------------------------------------------------------------------
# 9. An IRC sink: the channel notice, and highlight on/off.
# --------------------------------------------------------------------------


async def _wait_for_stdout_after(proc, pattern: str, after_idx: int, timeout: float = 30.0) -> str:
    """Like GnuworldProc.wait_for_stdout(), but only looks at lines that
    arrive after ``after_idx`` - wait_for_stdout() itself always rescans from
    the start of proc.stdout_lines, so a second wait for the same substring
    (e.g. "Connected", seen once per link) would return instantly on the
    first occurrence instead of waiting for a new one."""
    deadline = asyncio.get_event_loop().time() + timeout
    regex = re.compile(re.escape(pattern))
    while True:
        for line in proc.stdout_lines[after_idx:]:
            if regex.search(line):
                return line
        if asyncio.get_event_loop().time() > deadline:
            recent = "\n".join(proc.stdout_lines[-40:])
            raise TimeoutError(f"timed out waiting for {pattern!r}. Recent output:\n{recent}")
        await asyncio.sleep(0.2)


@pytest.mark.asyncio
async def test_irc_sink_delivers_a_notice_with_highlighted_value(docker_stack, fake_hub, tmp_path):
    _require_local()
    hub = fake_hub
    channel = "#logtest"
    ts = 1_700_000_000
    logging_conf = (
        "sink.chan.type = irc\n"
        f"sink.chan.channel = {channel}\n"
        "sink.chan.level = DEBUG\n"
        "sink.chan.highlight = yes\n"
        "logger.core = INFO, chan\n"
    )
    burst = [f"{hub.server_numnick} B {channel} {ts} +tn"]

    async with link_bare(
        docker_stack, hub, tmp_path, logging_conf=logging_conf, burst=burst,
    ) as (hub, proc, conf_dir):
        await proc.wait_for_stdout("Connected", timeout=30.0)

        # SIGHUP with the file untouched still reloads it and logs
        # "Reloaded <path>" on "core" - a record with a substituted value
        idx = len(hub.received)
        await send_gnuworld_signal(proc, signal.SIGHUP)
        notice = await _wait_for_notice(hub, channel, timeout=15.0, after=idx,
                                        contains="Reloaded")

        assert notice.startswith("[I] [core] "), notice
        assert "\x02" in notice, "the substituted file name was not bolded"
        assert "\r" not in notice and "\n" not in notice

        # Turn highlight off and reload again: the same kind of record, now
        # with no bold at all
        override_conf_keys(conf_dir / "logging.conf", {"sink.chan.highlight": "no"})
        idx2 = len(hub.received)
        await send_gnuworld_signal(proc, signal.SIGHUP)
        notice2 = await _wait_for_notice(hub, channel, timeout=15.0, after=idx2,
                                         contains="Reloaded")

        assert notice2.startswith("[I] [core] "), notice2
        assert "\x02" not in notice2


@pytest.mark.asyncio
async def test_irc_sink_says_once_that_its_channel_does_not_exist(docker_stack, fake_hub, tmp_path):
    """A channel nobody is in is a channel the network does not have: nothing
    can be sent there, and the other sinks are told so - once, however many
    records are lost to it and however often the file is reloaded."""
    _require_local()
    hub = fake_hub
    logging_conf = (
        "sink.console.type = console\n"
        "sink.chan.type = irc\n"
        "sink.chan.channel = #nobodyhere\n"
        "logger.root = INFO, console, chan\n"
    )

    async with link_bare(docker_stack, hub, tmp_path, logging_conf=logging_conf) as (
        hub, proc, conf_dir,
    ):
        await proc.wait_for_stdout("Completed net burst", timeout=30.0)

        # Two reloads: each logs records the sink cannot deliver, and each
        # replaces the sink with a new one for the same channel
        for _ in range(2):
            await send_gnuworld_signal(proc, signal.SIGHUP)
            await asyncio.sleep(1.0)

        said = [line for line in proc.stdout_lines
                if "#nobodyhere" in line and "does not exist" in line]

        assert len(said) == 1, said
        assert " WARN " in said[0] and " core " in said[0], said[0]

        # And not while the burst could still have brought the channel: the
        # records of start-up and of the burst are lost in silence
        lines = proc.stdout_lines
        burstDone = next(i for i, line in enumerate(lines) if "Completed net burst" in line)
        assert lines.index(said[0]) >= burstDone


# --------------------------------------------------------------------------
# 9b. "sink.<id>.rate" on an irc sink: beyond the rate a record is dropped,
#     however many records the logger it is on produces.
# --------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_irc_sink_rate_limit_caps_what_reaches_the_channel(
    docker_stack, fake_hub, tmp_path
):
    """A channel sink at 2/min beside a file sink with no limit: twenty records
    on one logger, all twenty in the file, at most two notices on the channel.

    The flood is twenty PRIVMSGs from a source numeric that does not exist, one
    WARN each on "core.proto" - and not a SIGHUP, which would reload
    logging.conf and so build a NEW sink with a full bucket every time.

    The notice that says how many records were suppressed needs the bucket to
    refill, which at this rate is thirty seconds away, so it is not waited for
    here: the count it carries is what test_logger_ratelimit covers.
    """
    _require_local()
    hub = fake_hub
    channel = "#logtest"
    ts = 1_700_000_000
    root = GnuworldProc.conf_root(tmp_path / "etc-gnuworld")
    log_path = f"{root}/core.log"
    logging_conf = (
        "sink.chan.type = irc\n"
        f"sink.chan.channel = {channel}\n"
        "sink.chan.level = DEBUG\n"
        "sink.chan.rate = 2/min\n"
        "sink.corelog.type = file\n"
        f"sink.corelog.path = {log_path}\n"
        "sink.corelog.format = json\n"
        "logger.core.proto = WARN, chan, corelog\n"
        "additivity.core.proto = no\n"
    )
    burst = [f"{hub.server_numnick} B {channel} {ts} +tn"]

    async with link_bare(
        docker_stack, hub, tmp_path, logging_conf=logging_conf, burst=burst,
    ) as (hub, proc, conf_dir):
        await proc.wait_for_stdout("Connected", timeout=30.0)
        await hub.drain_messages(timeout=1.0)

        sent = len(hub.received)

        for at in range(20):
            await hub.send_raw(f"ZZ{at:03d} P AzAAA :flood {at}")

        # The hub reads only when it is asked to, so this is what collects
        # whatever notices the twenty records above turned into
        await hub.drain_messages(timeout=1.0)

        notices = [text for text in
                   (_notice_text(line, channel) for line in hub.received[sent:])
                   if text is not None]

    records = [r for r in _read_json_lines(Path(log_path)) if r.get("logger") == "core.proto"]

    # Every record was logged; only two of them were sent to the channel
    assert len(records) == 20, records
    assert len(notices) <= 2, notices

    # A limit and not a mute: the bucket starts full, so the first records went
    assert notices, "the rate limit let nothing through at all"


# --------------------------------------------------------------------------
# 10. cservice is configured in logging.conf like every other logger: with no
#     line of its own its records walk up to the root, the five logging keys
#     it used to have are read no more, and the section bin/logging.example.conf
#     ships is what reproduces the outputs those keys used to give.
# --------------------------------------------------------------------------


# bin/logging.example.conf's cservice section, verbatim.  _cservice_section()
# checks each line is still in that file, so a change there is a failure here
# rather than a test quietly checking something the example no longer says.
CSERVICE_EXAMPLE_SECTION = """\
sink.cservice.type     = file
sink.cservice.path     = cservice.log
sink.cservice.format   = json
sink.debugchan.type    = irc
sink.debugchan.channel = #coder-com
sink.debugchan.level   = INFO
logger.cservice              = DEBUG, cservice, debugchan, console
additivity.cservice          = no
logger.cservice.commands     = INFO, cservice
additivity.cservice.commands = no
"""

# The five keys an install from before logging.conf still has in cservice.conf
CSERVICE_REMOVED_KEYS = (
    "log_verbosity = 5\n"
    "chan_verbosity = 4\n"
    "console_verbosity = 5\n"
    "log_sql = yes\n"
    "console_sql = yes\n"
)


def _cservice_section(conf_root: str) -> str:
    """The section above with its one file path made absolute, which is all a
    test changes about it: a bare "cservice.log" would land in the conf dir
    anyway (gnuworld runs there), but the test would then have to guess so."""
    example = (
        Path(__file__).resolve().parents[2] / "bin" / "logging.example.conf"
    ).read_text(encoding="utf-8")
    for line in CSERVICE_EXAMPLE_SECTION.splitlines():
        assert line in example, f"bin/logging.example.conf no longer has {line!r}"

    return CSERVICE_EXAMPLE_SECTION.replace(
        "= cservice.log", f"= {conf_root}/cservice.log",
    )


def _cservice_logging_conf(tmp_path, *, sql_debug: bool = False) -> tuple[str, Path, Path]:
    """A logging.conf with a JSON sink on the root, a console sink, and the
    cservice section bin/logging.example.conf ships - which is how cservice is
    configured now that it has no logging keys of its own.

    Returns the file's text, the path of cservice's own log, and the path of
    the root's.
    """
    root = GnuworldProc.conf_root(tmp_path / "etc-gnuworld")
    main_log = f"{root}/main.log"
    text = (
        "sink.mainlog.type = file\n"
        f"sink.mainlog.path = {main_log}\n"
        "sink.mainlog.format = json\n"
        "sink.console.type = console\n"
        "logger.root = INFO, mainlog\n"
        + _cservice_section(root)
        + ("logger.cservice.sql = DEBUG\n" if sql_debug else "")
    )
    return text, Path(f"{root}/cservice.log"), Path(main_log)


@pytest.mark.asyncio
async def test_cservice_without_a_logger_line_reaches_the_root_sinks(
    docker_stack, fake_hub_p11, tmp_path
):
    """No "logger.cservice" line: cservice writes no log of its own, and its
    records go to the sinks of the root like every other module's."""
    root = GnuworldProc.conf_root(tmp_path / "etc-gnuworld")
    log_path = f"{root}/main.log"
    logging_conf = (
        "sink.mainlog.type = file\n"
        f"sink.mainlog.path = {log_path}\n"
        "sink.mainlog.format = json\n"
        "logger.root = INFO, mainlog\n"
    )

    async with link_cservice_logging(
        docker_stack, fake_hub_p11, tmp_path, logging_conf=logging_conf,
    ) as (hub, proc, conf_dir):
        await asyncio.sleep(0.5)

    assert not (conf_dir / "cservice.log").exists(), sorted(p.name for p in conf_dir.iterdir())

    records = _read_json_lines(Path(log_path))
    assert any(r.get("logger") == "cservice" for r in records), \
        sorted({r.get("logger") for r in records})


@pytest.mark.asyncio
async def test_cservice_writes_no_log_of_its_own_without_a_logging_conf(
    docker_stack, fake_hub_p11, tmp_path
):
    """And with no logging.conf at all: still no cservice.log, and the records
    reach the one sink the built-in default is left with under the harness's
    own -D, the console."""
    async with link_cservice_logging(docker_stack, fake_hub_p11, tmp_path) as (
        hub, proc, conf_dir,
    ):
        await asyncio.sleep(0.5)
        console = list(proc.stdout_lines)

    assert not (conf_dir / "cservice.log").exists(), sorted(p.name for p in conf_dir.iterdir())
    assert any(
        CONSOLE_LINE.match(line) and re.search(r"\s+cservice\s+Channel join complete", line)
        for line in console
    ), [line for line in console if CONSOLE_LINE.match(line)][-20:]


@pytest.mark.asyncio
async def test_cservice_conf_with_the_removed_logging_keys_still_starts(
    docker_stack, fake_hub_p11, tmp_path
):
    """A cservice.conf that still carries the five keys starts normally and is
    not read for them: log_sql = yes asks for every statement and gets none."""
    root = GnuworldProc.conf_root(tmp_path / "etc-gnuworld")
    log_path = f"{root}/main.log"
    logging_conf = (
        "sink.mainlog.type = file\n"
        f"sink.mainlog.path = {log_path}\n"
        "sink.mainlog.format = json\n"
        "logger.root = INFO, mainlog\n"
    )

    async with link_cservice_logging(
        docker_stack, fake_hub_p11, tmp_path,
        logging_conf=logging_conf, cservice_extra=CSERVICE_REMOVED_KEYS,
    ) as (hub, proc, conf_dir):
        await asyncio.sleep(0.5)

    records = _read_json_lines(Path(log_path))
    assert records, "gnuworld logged nothing: did it start?"

    # Not read: no log of cservice's own, and no statement logged either
    assert not (conf_dir / "cservice.log").exists()
    assert not [
        r for r in records
        if r.get("logger") == "cservice.sql" and r.get("level") == "DEBUG"
    ]


# The four keys cservice used to page with, which are read no more either: a
# pager is a sink of logging.conf now.  The token is recognisable on purpose -
# nothing anywhere may print it.
PUSHOVER_TOKEN_IN_CONF = "SECRETtokenNobodyMayLog0123456789"
CSERVICE_REMOVED_PUSHOVER_KEYS = (
    "pushover_enable = yes\n"
    f"pushover_token = {PUSHOVER_TOKEN_IN_CONF}\n"
    "pushover_userkey = SECRETuserkeyNobodyMayLog01234567\n"
    "pushover_verbosity = 3\n"
)


@pytest.mark.asyncio
async def test_cservice_conf_with_the_removed_pushover_keys_still_starts(
    docker_stack, fake_hub_p11, tmp_path
):
    """A cservice.conf that still carries the four pushover keys starts
    normally, prints neither the token nor the user key, and pages nobody."""
    root = GnuworldProc.conf_root(tmp_path / "etc-gnuworld")
    log_path = f"{root}/main.log"
    logging_conf = (
        "sink.mainlog.type = file\n"
        f"sink.mainlog.path = {log_path}\n"
        "sink.mainlog.format = json\n"
        "logger.root = INFO, mainlog\n"
    )

    async with link_cservice_logging(
        docker_stack, fake_hub_p11, tmp_path,
        logging_conf=logging_conf, cservice_extra=CSERVICE_REMOVED_PUSHOVER_KEYS,
    ) as (hub, proc, conf_dir):
        await asyncio.sleep(0.5)
        console = list(proc.stdout_lines)

    records = _read_json_lines(Path(log_path))
    assert records, "gnuworld logged nothing: did it start?"

    # No record and no console line carries the token or the user key
    whole_log = Path(log_path).read_text(encoding="utf-8", errors="replace")
    assert "SECRET" not in whole_log, [l for l in whole_log.splitlines() if "SECRET" in l]
    assert not [line for line in console if "SECRET" in line]

    # "pushover_enable = yes" enables nothing any more: no notifier is attached,
    # so nothing is sent and nothing failed to be sent either
    assert not [r for r in records if r.get("logger", "").startswith("core.notifier")], \
        [r for r in records if r.get("logger", "").startswith("core.notifier")]


@pytest.mark.asyncio
async def test_cservice_example_section_gives_it_a_file_and_the_debug_channel(
    docker_stack, fake_hub_p11, tmp_path
):
    """bin/logging.example.conf's cservice section: its own JSON file, INFO and
    worse on the debug channel, the command log in the file only, and - by
    "additivity.cservice = no" - nothing of cservice in the root's own file."""
    hub = fake_hub_p11
    channel = "#coder-com"  # cservice.example.conf's debug_channel
    ts = 1_700_000_004
    burst = [f"{hub.server_numnick} B {channel} {ts} +tn"]
    logging_conf, cservice_log, main_log = _cservice_logging_conf(tmp_path)

    async with link_cservice_logging(
        docker_stack, hub, tmp_path, logging_conf=logging_conf, burst=burst,
    ) as (hub, proc, conf_dir):
        # An INFO record of "cservice" itself, on the channel, as a notice
        notice = await _wait_for_notice(hub, channel, timeout=30.0,
                                        contains="Channel join complete")
        assert notice.startswith("[I] [cservice] "), notice

        # One command, which is a record of "cservice.commands"
        await cs.login(hub)
        await asyncio.sleep(0.5)

        texts = [t for t in (_notice_text(line, channel) for line in hub.received)
                 if t is not None]

    records = _read_json_lines(cservice_log)
    assert records, sorted(p.name for p in conf_dir.iterdir())
    assert all({"timestamp", "level", "logger", "message"}.issubset(r.keys()) for r in records)
    assert any(r.get("logger") == "cservice" for r in records)

    logins = [
        r for r in records
        if r.get("logger") == "cservice.commands" and r.get("command") == "LOGIN"
    ]
    assert logins, sorted({r.get("logger") for r in records})
    assert logins[0].get("message") == "LOGIN by adminone", logins[0]

    # The command log is the file's alone: its sentence is noise on a channel
    assert not [t for t in texts if "[cservice.commands]" in t], texts
    assert not [t for t in texts if "LOGIN by" in t], texts

    # additivity.cservice = no: none of it walks up to the sinks of the root
    main_records = _read_json_lines(main_log)
    assert main_records
    assert not [
        r for r in main_records if str(r.get("logger", "")).startswith("cservice")
    ], [r for r in main_records if str(r.get("logger", "")).startswith("cservice")]

    # And with no "logger.cservice.sql" line, the statements are not logged
    assert not [
        r for r in records
        if r.get("logger") == "cservice.sql" and r.get("level") == "DEBUG"
    ]


@pytest.mark.asyncio
async def test_cservice_sql_debug_logs_every_statement_but_not_to_the_channel(
    docker_stack, fake_hub_p11, tmp_path
):
    """"logger.cservice.sql = DEBUG" logs every statement in full, into
    cservice.log - and the debug channel's own INFO level is what keeps those
    DEBUG records off a channel."""
    hub = fake_hub_p11
    channel = "#coder-com"
    ts = 1_700_000_005
    burst = [f"{hub.server_numnick} B {channel} {ts} +tn"]
    logging_conf, cservice_log, _main_log = _cservice_logging_conf(tmp_path, sql_debug=True)

    async with link_cservice_logging(
        docker_stack, hub, tmp_path, logging_conf=logging_conf, burst=burst,
    ) as (hub, proc, conf_dir):
        await _wait_for_notice(hub, channel, timeout=30.0, contains="Channel join complete")
        await asyncio.sleep(0.5)

        texts = [t for t in (_notice_text(line, channel) for line in hub.received)
                 if t is not None]

    records = _read_json_lines(cservice_log)
    queries = [
        r for r in records
        if r.get("logger") == "cservice.sql" and r.get("level") == "DEBUG" and "query" in r
    ]
    assert queries, sorted({r.get("logger") for r in records})

    # Not one of them on the channel: a statement is never for a channel
    assert not [t for t in texts if "[cservice.sql]" in t], texts


# --------------------------------------------------------------------------
# 10b. The command log is a logger of its own, and its sentence carries no
#      arguments: neither the debug channel nor the console may see the
#      e-mail address of a HELLO or the pattern of a SCANEMAIL.
# --------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_cservice_command_log_keeps_its_arguments_out_of_the_channel(
    docker_stack, fake_hub_p11, tmp_path
):
    hub = fake_hub_p11
    channel = "#coder-com"  # cservice.example.conf's debug_channel
    ts = 1_700_000_003
    argument = "nobody-42@scan.example.invalid"
    burst = [f"{hub.server_numnick} B {channel} {ts} +tn"]
    logging_conf, cservice_log, _main_log = _cservice_logging_conf(tmp_path)

    async with link_cservice_logging(
        docker_stack, hub, tmp_path, logging_conf=logging_conf, burst=burst,
    ) as (hub, proc, conf_dir):
        # The channel sink is live: a record of "cservice" itself does reach the
        # debug channel, which is what the command log must not do
        await _wait_for_notice(hub, channel, timeout=30.0, contains="Channel join complete")

        admin = await cs.login(hub)
        replies = await cs.run(hub, admin, f"scanemail {argument}")
        assert any("Found 0 matches" in line for line in replies), replies

        await asyncio.sleep(0.5)

        texts = [_notice_text(line, channel) for line in hub.received]
        console = list(proc.stdout_lines)

    records = _read_json_lines(cservice_log)

    commands = [r for r in records if r.get("logger") == "cservice.commands"]
    assert commands, sorted({r.get("logger") for r in records})

    scans = [r for r in commands if r.get("command") == "SCANEMAIL"]
    assert len(scans) == 1, scans
    record = scans[0]

    # The whole line is a field of the JSON record, and the sentence is not it
    assert record.get("command_line") == f"SCANEMAIL {argument}"
    assert argument not in record.get("message", "")
    assert record.get("message") == "SCANEMAIL by adminone", record

    # And nothing carried it to the debug channel or to a console log record.
    # The raw protocol dump of -L is not one: the message the user sent is on
    # the wire whatever the logging system then makes of it.
    assert not [t for t in texts if t is not None and argument in t]
    assert any(CONSOLE_LINE.match(line) for line in console), "no log record on the console at all"
    assert not [line for line in console if CONSOLE_LINE.match(line) and argument in line]


# --------------------------------------------------------------------------
# 11. A "logger.cservice" line with a sink of its own routes the module's
#     records there, at the level that line asks for.
# --------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_cservice_configured_mode_uses_logging_conf(docker_stack, fake_hub_p11, tmp_path):
    _require_local()
    root = GnuworldProc.conf_root(tmp_path / "etc-gnuworld")
    log_path = f"{root}/main.log"
    logging_conf = (
        "sink.mainlog.type = file\n"
        f"sink.mainlog.path = {log_path}\n"
        "sink.mainlog.format = json\n"
        "logger.root = INFO, mainlog\n"
        "logger.cservice = DEBUG, mainlog\n"
    )

    async with link_cservice_logging(
        docker_stack, fake_hub_p11, tmp_path, logging_conf=logging_conf,
    ) as (hub, proc, conf_dir):
        await cs.login(hub)
        await asyncio.sleep(0.5)

    assert not (conf_dir / "cservice.log").exists()

    records = _read_json_lines(Path(log_path))
    assert any(r.get("logger") == "cservice" for r in records)

    # The sub-loggers of cservice have no line of their own here: they inherit
    # that level, and are additive, so the command log lands in the same file
    assert any(r.get("logger") == "cservice.commands" for r in records), \
        sorted({r.get("logger") for r in records})


# --------------------------------------------------------------------------
# 12. A forced SQL error in cservice: cservice.sql gets an ERROR record with
#     non-empty error and query fields. The table is always renamed back.
# --------------------------------------------------------------------------


def _psql(sql: str, db: str = "cservice") -> None:
    subprocess.run(
        ["docker", "compose", "-f", str(COMPOSE_FILE), "exec", "-T", "postgres",
         "psql", "-U", "gnuworld", "-d", db, "-c", sql],
        cwd=str(HARNESS_DIR),
        check=True,
        capture_output=True,
    )


@pytest.fixture
def broken_webnotices_table(docker_stack):
    """Renames "webnotices" away so cservice's start-up DELETE FROM it fails,
    and always renames it back - the database is shared by the whole
    session."""
    require_module("cservice")
    docker_stack.up()
    _psql("ALTER TABLE webnotices RENAME TO webnotices_moved")
    try:
        yield
    finally:
        _psql("ALTER TABLE webnotices_moved RENAME TO webnotices")


@pytest.fixture
def blocked_user_commit(docker_stack):
    """Makes every "UPDATE users" of a client whose nick begins with "sqlfail"
    fail, and always takes the trigger away again - the database is shared by
    the whole session.

    A trigger rather than a constraint: a check constraint violation reports
    the whole failing row, password hash and all, in the message of the
    database, which is not what the case below is about.
    """
    require_module("cservice")
    docker_stack.up()
    _psql(
        "CREATE OR REPLACE FUNCTION harness_block_user_commit() RETURNS trigger "
        "LANGUAGE plpgsql AS $$ BEGIN "
        "IF NEW.last_updated_by LIKE 'sqlfail%' THEN "
        "RAISE EXCEPTION 'harness: this users update is blocked'; END IF; "
        "RETURN NEW; END $$; "
        "CREATE TRIGGER harness_block_user_commit BEFORE UPDATE ON users "
        "FOR EACH ROW EXECUTE PROCEDURE harness_block_user_commit()"
    )
    try:
        yield
    finally:
        _psql(
            "DROP TRIGGER IF EXISTS harness_block_user_commit ON users; "
            "DROP FUNCTION IF EXISTS harness_block_user_commit()"
        )


@pytest.mark.asyncio
async def test_a_statement_carrying_a_credential_is_kept_out_of_the_logs(
    docker_stack, fake_hub_p11, tmp_path, blocked_user_commit
):
    """sqlUser::commit() writes the password, the TOTP key and the SCRAM record
    of a user.  Its statement is executed with Exec(query, false), so no DEBUG
    record shows it, and the ERROR record of a failed one never carries a
    statement - even under "logger.cservice.sql = DEBUG", which logs every other
    statement in full."""
    # doc/cservice.addme.sql: what "Admin" has in the password column
    password_hash = "xEDi1V791f7bddc526de7e3b0602d0b2993ce21d"
    logging_conf, cservice_log, _main_log = _cservice_logging_conf(tmp_path, sql_debug=True)

    async with link_cservice_logging(
        docker_stack, fake_hub_p11, tmp_path, logging_conf=logging_conf,
    ) as (hub, proc, conf_dir):
        # The nick is what the trigger above looks at: this client's commits fail
        admin = await cs.login(hub, nick="sqlfail")
        replies = await cs.run(hub, admin, "set lang EN")
        assert any("Language is set to" in line for line in replies), replies

        await asyncio.sleep(0.5)

    text = cservice_log.read_text(encoding="utf-8", errors="replace")
    records = _read_json_lines(cservice_log)

    # Every statement is logged, so the ones that carry nothing secret are there
    logged = [r for r in records if r.get("logger") == "cservice.sql" and r.get("level") == "DEBUG"]
    assert logged, sorted({r.get("logger") for r in records})

    # The failure is reported, by what the database said and nothing else
    errors = [r for r in records if r.get("logger") == "cservice.sql" and r.get("level") == "ERROR"]
    blocked = [e for e in errors if "blocked" in e.get("error", "")]
    assert blocked, errors
    assert not any("query" in e for e in errors), errors

    # The database's message is its primary line only: PQerrorMessage()'s
    # "LINE 1: <statement>" excerpt and its "DETAIL: Key (...)=(...)" would
    # each put the statement's literal values back into the record that the
    # Exec(query, false) above kept them out of
    for error in errors:
        reported = error.get("error", "")
        assert "LINE 1" not in reported, error
        assert "password =" not in reported, error
        assert "DETAIL:" not in reported, error

    # And the hash the statement carried is nowhere in the file at all
    assert password_hash not in text


@pytest.mark.asyncio
async def test_forced_sql_error_in_cservice(
    docker_stack, fake_hub_p11, tmp_path, broken_webnotices_table
):
    """A failed statement is an ERROR record of "cservice.sql" whatever the
    logger is configured at: its code default reports errors and nothing else.

    This is also the case whose error PostgreSQL does report a position for -
    a missing relation - so its full PQerrorMessage() carries a "LINE 1:"
    excerpt of the statement.  The record holds the primary message alone."""
    logging_conf, cservice_log, _main_log = _cservice_logging_conf(tmp_path)

    async with link_cservice_logging(
        docker_stack, fake_hub_p11, tmp_path, logging_conf=logging_conf,
    ) as (hub, proc, conf_dir):
        await asyncio.sleep(1.0)

    records = _read_json_lines(cservice_log)
    errors = [r for r in records if r.get("logger") == "cservice.sql" and r.get("level") == "ERROR"]
    assert errors, records
    assert any(
        e.get("message", "").startswith("SQL Error:") and e.get("error")
        for e in errors
    )

    # The one the fixture causes, and no excerpt of the statement behind it
    missing = [e for e in errors if "webnotices" in e.get("error", "")]
    assert missing, errors
    for error in errors:
        reported = error.get("error", "")
        assert "LINE 1" not in reported, error
        assert "DELETE FROM" not in reported, error

    # One record for the one failed statement: the handle reports it, and no
    # caller reports it a second time
    assert len(missing) == 1, missing

    # Naming the function that ran the statement, not the handle's own Exec:
    # its std::source_location argument is defaulted, so it is the call site's
    assert missing[0].get("function") == "cservice::OnAttach", missing
    assert not any("pgsqlDB" in e.get("function", "") for e in errors), errors


# --------------------------------------------------------------------------
# 13. ccontrol: ccontrol.sql DEBUG records reach the root's own sink, only
#     when logging.conf asks for them.
# --------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_ccontrol_sql_debug_records_reach_the_root_sink(docker_stack, fake_hub, tmp_path):
    _require_local()
    root = GnuworldProc.conf_root(tmp_path / "etc-gnuworld")
    log_path = f"{root}/main.log"
    logging_conf = (
        "sink.mainlog.type = file\n"
        f"sink.mainlog.path = {log_path}\n"
        "sink.mainlog.format = json\n"
        "logger.root = INFO, mainlog\n"
        "logger.ccontrol.sql = DEBUG\n"
    )

    async with link_ccontrol_logging(
        docker_stack, fake_hub, tmp_path, logging_conf=logging_conf,
    ) as (hub, proc, conf_dir):
        await asyncio.sleep(1.0)

    records = _read_json_lines(Path(log_path))
    queries = [
        r for r in records
        if r.get("logger") == "ccontrol.sql" and r.get("level") == "DEBUG" and "query" in r
    ]
    assert queries, "no query DEBUG record on ccontrol.sql with logger.ccontrol.sql = DEBUG"


@pytest.mark.asyncio
async def test_ccontrol_sql_debug_records_absent_without_the_logger_line(
    docker_stack, fake_hub, tmp_path
):
    _require_local()
    root = GnuworldProc.conf_root(tmp_path / "etc-gnuworld")
    log_path = f"{root}/main.log"
    logging_conf = (
        "sink.mainlog.type = file\n"
        f"sink.mainlog.path = {log_path}\n"
        "sink.mainlog.format = json\n"
        "logger.root = INFO, mainlog\n"
    )

    async with link_ccontrol_logging(
        docker_stack, fake_hub, tmp_path, logging_conf=logging_conf,
    ) as (hub, proc, conf_dir):
        await asyncio.sleep(1.0)

    records = _read_json_lines(Path(log_path))
    queries = [r for r in records if r.get("logger") == "ccontrol.sql" and "query" in r]
    assert not queries, queries


@pytest.fixture
def broken_badchannels_table(docker_stack):
    """Renames ccontrol's "badchannels" away so its start-up SELECT from it
    fails, and always renames it back - the database is shared by the whole
    session."""
    require_module("ccontrol")
    docker_stack.up()
    _psql("ALTER TABLE badchannels RENAME TO badchannels_moved", db="ccontrol")
    try:
        yield
    finally:
        _psql("ALTER TABLE badchannels_moved RENAME TO badchannels", db="ccontrol")


@pytest.mark.asyncio
async def test_a_failed_statement_reports_itself_in_a_module_that_never_did(
    docker_stack, fake_hub, tmp_path, broken_badchannels_table
):
    """ccontrol::loadBadChannels() reports nothing of its own: it returns false
    and its one caller ignores that.  The handle reports the failure anyway, on
    "ccontrol.sql" and naming that function - which is what every module gets
    out of Exec() taking the caller's location."""
    _require_local()
    root = GnuworldProc.conf_root(tmp_path / "etc-gnuworld")
    log_path = f"{root}/main.log"
    logging_conf = (
        "sink.mainlog.type = file\n"
        f"sink.mainlog.path = {log_path}\n"
        "sink.mainlog.format = json\n"
        "logger.root = INFO, mainlog\n"
    )

    async with link_ccontrol_logging(
        docker_stack, fake_hub, tmp_path, logging_conf=logging_conf,
    ) as (hub, proc, conf_dir):
        await asyncio.sleep(1.0)

    records = _read_json_lines(Path(log_path))
    errors = [r for r in records if r.get("logger") == "ccontrol.sql" and r.get("level") == "ERROR"]
    missing = [e for e in errors if "badchannels" in e.get("error", "")]
    assert len(missing) == 1, errors
    assert missing[0].get("message", "").startswith("SQL Error:"), missing
    assert missing[0].get("function") == "ccontrol::loadBadChannels", missing

    # Which statement of that function it was: the line of the call
    source = (Path(__file__).resolve().parents[2] / "mod.ccontrol" / "ccontrol.cc").read_text(
        encoding="utf-8", errors="replace").splitlines()
    line = missing[0].get("line")
    assert isinstance(line, int) and "Exec(" in "".join(source[line - 3:line]), missing

    # The statement is not part of the record here either
    assert "query" not in missing[0], missing
    assert "SELECT" not in missing[0].get("error", ""), missing


# --------------------------------------------------------------------------
# 14. Reconnect cycle with an IRC sink configured: guards
#     IrcLogSink::forgetServer(). Slow: a mandatory 10s pause is part of
#     gnuworld's own reconnect logic (src/main.cc).
# --------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.timeout(90)
async def test_reconnect_cycle_with_irc_sink_survives(docker_stack, fake_hub, tmp_path):
    _require_local()
    hub = fake_hub
    channel = "#logtest2"
    ts = 1_700_000_001
    logging_conf = (
        "sink.chan.type = irc\n"
        f"sink.chan.channel = {channel}\n"
        "sink.chan.level = INFO\n"
        "logger.core = INFO, chan\n"
    )
    burst = [f"{hub.server_numnick} B {channel} {ts} +tn"]

    async with link_bare(
        docker_stack, hub, tmp_path, logging_conf=logging_conf, auto_reconnect=True, burst=burst,
    ) as (hub, proc, conf_dir):
        await proc.wait_for_stdout("Connected", timeout=30.0)
        after_first_connect = len(proc.stdout_lines)

        # Drop the connection without tearing down the hub's listening
        # socket, so gnuworld's reconnect attempt (its own xServer destroyed
        # and rebuilt, IrcLogSink::forgetServer along the way) can land
        # somewhere - real link loss, not FakeHub.close()'s full teardown.
        await drop_connection(hub)

        # gnuworld notices, destroys its xServer, sleeps 10s (src/main.cc),
        # and reconnects; the fake hub can accept a fresh connection because
        # only the transport was dropped, not the listening socket.
        await hub.accept_and_handshake(timeout=30.0, burst=burst)
        await _wait_for_stdout_after(proc, "Connected", after_first_connect, timeout=30.0)

        assert proc.proc.returncode is None, "gnuworld did not survive the reconnect"

        # And it logs to the channel again, through a freshly configured IRC
        # sink bound to the new server object
        idx = len(hub.received)
        await send_gnuworld_signal(proc, signal.SIGHUP)
        notice = await _wait_for_notice(hub, channel, timeout=15.0, after=idx,
                                        contains="Reloaded")
        assert notice.startswith("[I] [core] ")


# --------------------------------------------------------------------------
# 15. Module unload/reload with an extractor-bearing record: SKIPPED.
#
# The harness has no helper to unload/reload a module (conftest.py has no
# such fixture, and none of the existing tests do this), and neither
# mod.ccontrol nor mod.cservice exposes an UNLOAD/LOAD chat command
# (`git grep -ln 'UNLOAD\|"LOAD"' mod.ccontrol mod.cservice` finds nothing).
# The only reachable unload/reload path in the tree is mod.gnutest's own
# "reload" command (mod.gnutest/gnutest.cc:382-383), which reloads gnutest
# itself, not cservice - it cannot produce a record carrying a sqlUser
# extractor, which only mod.cservice's classes ever register. Per this
# task's instructions, this case is skipped rather than adding a new
# unload/reload trigger to production code.
# --------------------------------------------------------------------------


# --------------------------------------------------------------------------
# 16. A module logs on its own logger, not on "legacy": mod.gnutest.
# --------------------------------------------------------------------------


@asynccontextmanager
async def _link_gnutest_logging(docker_stack, hub, tmp_path, logging_conf: str):
    """gnuworld with mod.gnutest and a logging.conf of the test's own.

    link_debug() loads gnutest but takes no logging.conf, and link_bare()
    takes one but loads no module; this is the two together, and nothing
    else - mod.debug is left out because nothing here needs it.
    """
    require_module("gnutest")
    conf_dir = _prepare_conf_dir(tmp_path)
    root = GnuworldProc.conf_root(conf_dir)
    GnuworldProc.write_gnutest_config(conf_dir / "gnutest.conf")
    GnuworldProc.write_config(
        conf_dir / "GNUWorld.conf", uplink=CONTAINER_UPLINK, port=hub.port,
        password=hub.password,
        module_lines=f"module = libgnutest.la {root}/gnutest.conf",
    )
    (conf_dir / "logging.conf").write_text(logging_conf, encoding="utf-8")
    with (conf_dir / "GNUWorld.conf").open("a", encoding="utf-8") as fh:
        fh.write(f"\nlogging_conf = {root}/logging.conf\n")

    proc = GnuworldProc(conf_dir=conf_dir)
    await proc.start()
    try:
        await hub.accept_and_handshake(timeout=45.0)
        await proc.wait_for_stdout("Connected", timeout=30.0)
        yield hub, proc, conf_dir
    finally:
        await proc.terminate()


@pytest.mark.asyncio
async def test_gnutest_spawn_logs_on_its_own_logger_and_not_on_legacy(
    docker_stack, fake_hub, tmp_path
):
    """Spawning a fake server and a fake client through gnutest's chat
    commands: each is an INFO record on the logger "gnutest", carrying the
    object's own fields (server_name, client_nick) because the statement
    names the object with .with(). Nothing of it is on "legacy", which is
    where every one of these lines was while gnutest wrote to elog."""
    _require_local()
    hub = fake_hub
    root = GnuworldProc.conf_root(tmp_path / "etc-gnuworld")
    log_path = f"{root}/main.log"
    logging_conf = (
        "sink.mainlog.type = file\n"
        f"sink.mainlog.path = {log_path}\n"
        "sink.mainlog.format = json\n"
        "logger.root = DEBUG, mainlog\n"
        "logger.legacy = DEBUG\n"
    )

    async with _link_gnutest_logging(docker_stack, hub, tmp_path, logging_conf) as (
        hub, proc, conf_dir,
    ):
        asker = await hub.introduce_nick("asker", username="asker")
        await gt.run(hub, asker, "spawnserver spawned.testnet a spawned server")
        await gt.run(hub, asker, "spawnclient fakey spawned.testnet")
        await asyncio.sleep(0.3)  # let the sink's writes land

    records = _read_json_lines(Path(log_path))
    assert records, "the JSON sink never received a record"

    gnutest_records = [r for r in records if r.get("logger") == "gnutest"]
    assert gnutest_records, "mod.gnutest logged nothing on the logger \"gnutest\""

    server_added = [
        r for r in gnutest_records
        if r.get("level") == "INFO" and r.get("server_name") == "spawned.testnet"
    ]
    assert server_added, [r["message"] for r in gnutest_records]
    assert "spawned.testnet" in server_added[0]["message"]
    assert server_added[0]["function"] == "gnutest::spawnServer"

    client_added = [
        r for r in gnutest_records
        if r.get("level") == "INFO" and r.get("client_nick") == "fakey"
    ]
    assert client_added, [r["message"] for r in gnutest_records]
    assert "fakey" in client_added[0]["message"]
    assert client_added[0]["function"] == "gnutest::spawnClient"

    # And none of it went out on the elog stream's logger.  "no legacy record
    # mentions it" would also hold with no legacy record at all, so the same
    # property is asserted from the other side as well: every record in the
    # file that mentions the spawn is one of gnutest's own, and that set is
    # not empty (the two above are in it).  "logger.legacy = DEBUG" is in the
    # fixture's logging.conf, so a record of the elog stream would be here.
    spoken_of = [
        r for r in records
        if "spawned.testnet" in r.get("message", "") or "fakey" in r.get("message", "")
    ]
    assert spoken_of, [r.get("message") for r in records]
    assert all(r.get("logger") == "gnutest" for r in spoken_of), [
        (r.get("logger"), r.get("message")) for r in spoken_of
    ]
    assert not any(r.get("logger") == "legacy" for r in spoken_of), spoken_of

    # The hand-written "Class::method> " prefix was the elog form of these
    # lines and the macro captures the function now, so it is nowhere at all
    text = Path(log_path).read_text(encoding="utf-8", errors="replace")
    assert "gnutest::spawnServer>" not in text
    assert "gnutest::spawnClient>" not in text
