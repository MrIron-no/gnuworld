# Recorded server-to-server traffic

Real traffic recorded from a live ircu hub with `../../p11-env`. Each file is
gnuworld's inbound stream as written by `gnuworld -l`: one message per line,
prefixed with the Unix time it was received. The `PASS` line is stripped.

## `p11-burst-scenario.log`

A full P11 net burst, as sent by a hub on ircu2 `p11-integration` (5856fbc) to a
freshly linked gnuworld, after `p11-env/scenario.py` had built its channels.
`OPLEVELS` was off, so ops are burst as `:o` and there are no digit op levels.

| Numeric | Nick | | Numeric | Nick |
|---|---|---|---|---|
| `ACAAJ` | OpTwo | | `ACAAO` | PlainOne |
| `ACAAK` | HidOne | | `ACAAP` | OpVoice |
| `ACAAL` | HidThree | | `ACAAQ` | VoiceOne |
| `ACAAM` | HidTwo | | `ACAAR` | OpOne |
| `ACAAN` | PlainTwo | | | |

What it covers:

- `SERVER ... J11`, the `CAP` line, and `N` lines carrying `@time=` message tags.
- `#p11-bans`: three ban triples (`mask timestamp setter`) from two setters, and
  the mode block `+tnlk 25 sekrit` (limit before key).
- `#p11-plain`: status-less members, `:v`, `:o`, and op plus voice sent as `:vo`.
- `#p11-deljoin`: a `:d` group of three on a `+D` channel.
- `#p11-wasdel`: a `:d` group on a channel that is no longer `+D`.

The `:d` groups include PlainOne and PlainTwo although both had spoken, which
reveals them on their own server. The hub never saw those messages (it routes
channel messages only toward servers with members on the channel), so its view
of who is hidden was stale when it sent this burst.
