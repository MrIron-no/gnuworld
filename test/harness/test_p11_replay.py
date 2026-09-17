"""Characterisation: the state gnuworld builds from a recorded P11 net burst.

Replays data/captures/p11-burst-scenario.log, real traffic from an ircu hub,
and asserts on gnuworld's resulting view of every channel through mod.debug.

This pins behaviour down ahead of the core refactor. It reads state, not code,
so it must keep passing unchanged while the burst and mode parsing underneath
is replaced. If it has to be edited, that is a behaviour change: say why.

The numeric to nick table is in data/captures/README.md.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from conftest import link_debug
from debugquery import chaninfo
from fake_hub import load_capture
from p10 import p10_token

CAPTURE = Path(__file__).parent / "data" / "captures" / "p11-burst-scenario.log"
PERMIT_ACCOUNT = "MrIron"


async def _asker(hub) -> str:
    """A client mod.debug will answer. Numbered under the hub (AB...), so it
    cannot collide with the recorded clients, which are all on the leaf (AC...)."""
    asker = await hub.introduce_nick("asker", username="asker")
    await hub.send_account(asker, PERMIT_ACCOUNT)
    return asker


@pytest.mark.asyncio
async def test_state_after_recorded_burst(docker_stack, fake_hub_p11, tmp_path):
    burst, _after = load_capture(CAPTURE)
    assert sum(p10_token(line) == "B" for line in burst) == 4, "fixture changed shape"

    async with link_debug(fake_hub_p11, tmp_path, burst=burst) as (hub, _proc):
        asker = await _asker(hub)

        plain = await chaninfo(hub, asker, "#p11-plain")
        assert plain.found
        assert plain.created == 1789637405
        assert plain.modes == "nt"
        assert (plain.key, plain.limit) == (None, None)
        assert plain.topic == "P11 capture scenario: plain channel"
        assert plain.bans == set()
        assert plain.members == {
            "PlainTwo": "none",
            "PlainOne": "none",
            "VoiceOne": "+v",
            "OpTwo": "+o",
            "OpOne": "+o",  # suffix-less, inherits ":o" from OpTwo
            "OpVoice": "+o+v",  # sent as ":vo"
        }

        bans = await chaninfo(hub, asker, "#p11-bans")
        assert bans.created == 1789637417
        assert bans.modes == "klnt"
        assert (bans.key, bans.limit) == ("sekrit", 25)  # burst as "+tnlk 25 sekrit"
        assert bans.members == {"OpTwo": "+o", "OpOne": "+o"}
        # P11 sends "mask timestamp setter" per ban; only the mask is a ban
        assert bans.bans == {"nasty*!*@*", "*!*lamer@*.example.org", "*!*@spam.example.net"}

        deljoin = await chaninfo(hub, asker, "#p11-deljoin")
        assert deljoin.created == 1789637434
        assert deljoin.modes == "Dnt"
        assert deljoin.members == {
            "PlainOne": "none",  # joined hidden, then spoke: no longer in ":d"
            "HidTwo": "hide",
            "HidOne": "hide",  # suffix-less, inherits ":d" from HidTwo
            "VoiceOne": "+v",
            "OpOne": "+o",
        }

        wasdel = await chaninfo(hub, asker, "#p11-wasdel")
        assert wasdel.created == 1789637442
        assert wasdel.modes == "nt"  # no longer +D, yet still has a hidden member
        assert wasdel.members == {"PlainTwo": "none", "HidThree": "hide", "OpTwo": "+o"}


@pytest.mark.asyncio
async def test_state_after_recorded_live_traffic(docker_stack, fake_hub_p11, tmp_path):
    burst, after = load_capture(CAPTURE)
    reveal = [line for line in after if p10_token(line) == "RV"]
    quits = [line for line in after if p10_token(line) == "Q"]
    assert (len(reveal), len(quits)) == (1, 9), "fixture changed shape"

    async with link_debug(fake_hub_p11, tmp_path, burst=burst) as (hub, _proc):
        asker = await _asker(hub)

        for line in reveal:
            await hub.send_raw(line)
        members = (await chaninfo(hub, asker, "#p11-deljoin")).members
        assert (members["HidOne"], members["HidTwo"]) == ("none", "hide")

        # Every recorded client quits, which must take all four channels away
        for line in quits:
            await hub.send_raw(line)
        for channel in ("#p11-plain", "#p11-bans", "#p11-deljoin", "#p11-wasdel"):
            assert not (await chaninfo(hub, asker, channel)).found
