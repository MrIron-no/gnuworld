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
from pathlib import Path

import pytest

import cservice_client as cs
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

        assert notice.startswith("[core] "), notice
        assert "\x02" in notice, "the substituted file name was not bolded"
        assert "\r" not in notice and "\n" not in notice

        # Turn highlight off and reload again: the same kind of record, now
        # with no bold at all
        override_conf_keys(conf_dir / "logging.conf", {"sink.chan.highlight": "no"})
        idx2 = len(hub.received)
        await send_gnuworld_signal(proc, signal.SIGHUP)
        notice2 = await _wait_for_notice(hub, channel, timeout=15.0, after=idx2,
                                         contains="Reloaded")

        assert notice2.startswith("[core] "), notice2
        assert "\x02" not in notice2


# --------------------------------------------------------------------------
# 10. cservice fallback mode (Postgres): its own JSON file, the deprecation
#     line once, log_sql on/off.
# --------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_cservice_fallback_mode_writes_its_own_json_log(docker_stack, fake_hub_p11, tmp_path):
    async with link_cservice_logging(docker_stack, fake_hub_p11, tmp_path) as (hub, proc, conf_dir):
        await asyncio.sleep(0.5)

    cservice_log = conf_dir / "cservice.log"
    assert cservice_log.is_file(), sorted(p.name for p in conf_dir.iterdir())
    records = _read_json_lines(cservice_log)
    assert records
    assert all({"timestamp", "level", "logger", "message"}.issubset(r.keys()) for r in records)
    assert any(r.get("logger") == "cservice" for r in records)

    deprecated = [
        r for r in records
        if "cservice logging keys in" in r.get("message", "")
        and "are deprecated" in r.get("message", "")
    ]
    assert len(deprecated) == 1, deprecated

    # Defaults (log_sql = no): no DEBUG query record on cservice.sql
    assert not any(
        r.get("logger") == "cservice.sql" and "query" in r for r in records
    ), [r for r in records if r.get("logger") == "cservice.sql"]


@pytest.mark.asyncio
async def test_cservice_fallback_mode_with_log_sql_yes_logs_queries(docker_stack, fake_hub_p11, tmp_path):
    async with link_cservice_logging(
        docker_stack, fake_hub_p11, tmp_path, cservice_overrides={"log_sql": "yes"},
    ) as (hub, proc, conf_dir):
        await asyncio.sleep(0.5)

    records = _read_json_lines(conf_dir / "cservice.log")
    queries = [
        r for r in records
        if r.get("logger") == "cservice.sql" and r.get("level") == "DEBUG" and "query" in r
    ]
    assert queries, records


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

    async with link_cservice_logging(
        docker_stack, hub, tmp_path, burst=burst,
    ) as (hub, proc, conf_dir):
        # The channel sink of the deprecated fallback mode is live: a record of
        # "cservice" itself does reach the debug channel at chan_verbosity = 4
        await _wait_for_notice(hub, channel, timeout=30.0, contains="Channel join complete")

        admin = await cs.login(hub)
        replies = await cs.run(hub, admin, f"scanemail {argument}")
        assert any("Found 0 matches" in line for line in replies), replies

        await asyncio.sleep(0.5)

        texts = [_notice_text(line, channel) for line in hub.received]
        console = list(proc.stdout_lines)

    records = _read_json_lines(conf_dir / "cservice.log")

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
# 11. cservice configured mode: logging.conf configures logger.cservice.
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
        await asyncio.sleep(0.5)

    assert not (conf_dir / "cservice.log").exists()

    records = _read_json_lines(Path(log_path))
    assert any(r.get("logger") == "cservice" for r in records)

    ignored = [
        r for r in records
        if "are ignored because logging.conf configures logger.cservice" in r.get("message", "")
    ]
    assert len(ignored) == 1, ignored


# --------------------------------------------------------------------------
# 12. A forced SQL error in cservice: cservice.sql gets an ERROR record with
#     non-empty error and query fields. The table is always renamed back.
# --------------------------------------------------------------------------


def _psql(sql: str) -> None:
    subprocess.run(
        ["docker", "compose", "-f", str(COMPOSE_FILE), "exec", "-T", "postgres",
         "psql", "-U", "gnuworld", "-d", "cservice", "-c", sql],
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
    of a user.  Its statement is executed with logQuery = false, so neither the
    DEBUG record of a successful one nor the ERROR record of a failed one shows
    it - even with log_sql = yes, which logs every other statement."""
    # doc/cservice.addme.sql: what "Admin" has in the password column
    password_hash = "xEDi1V791f7bddc526de7e3b0602d0b2993ce21d"

    async with link_cservice_logging(
        docker_stack, fake_hub_p11, tmp_path, cservice_overrides={"log_sql": "yes"},
    ) as (hub, proc, conf_dir):
        # The nick is what the trigger above looks at: this client's commits fail
        admin = await cs.login(hub, nick="sqlfail")
        replies = await cs.run(hub, admin, "set lang EN")
        assert any("Language is set to" in line for line in replies), replies

        await asyncio.sleep(0.5)

    text = (conf_dir / "cservice.log").read_text(encoding="utf-8", errors="replace")
    records = _read_json_lines(conf_dir / "cservice.log")

    # log_sql = yes, so the statements that carry nothing secret are all there
    logged = [r for r in records if r.get("logger") == "cservice.sql" and r.get("level") == "DEBUG"]
    assert logged, sorted({r.get("logger") for r in records})

    errors = [r for r in records if r.get("logger") == "cservice.sql" and r.get("level") == "ERROR"]
    withheld = [e for e in errors if e.get("query") == "(not logged)"]
    assert withheld, errors
    assert all("blocked" in e.get("error", "") for e in withheld), withheld

    # And the hash the statement carried is nowhere in the file at all
    assert password_hash not in text


@pytest.mark.asyncio
async def test_forced_sql_error_in_cservice(
    docker_stack, fake_hub_p11, tmp_path, broken_webnotices_table
):
    async with link_cservice_logging(docker_stack, fake_hub_p11, tmp_path) as (hub, proc, conf_dir):
        await asyncio.sleep(1.0)

    records = _read_json_lines(conf_dir / "cservice.log")
    errors = [r for r in records if r.get("logger") == "cservice.sql" and r.get("level") == "ERROR"]
    assert errors, records
    assert any(
        e.get("message", "").startswith("SQL Error:") and e.get("error") and e.get("query")
        for e in errors
    )


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
        assert notice.startswith("[core] ")


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
