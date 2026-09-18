# Recorded server-to-server traffic

Real traffic recorded from a live ircu hub with `../../p11-env`. Each file is
gnuworld's inbound stream as written by `gnuworld -l`: one message per line,
prefixed with the Unix time it was received. The `PASS` line is stripped.

## `p11-burst-scenario.log`

A full P11 net burst followed by a live reveal, as sent by a hub on ircu2
`p11-integration` (9fe8642, the first with the REVEAL token) to a freshly linked
gnuworld, after `p11-env/scenario.py` had built its channels.

| Numeric | Nick | | Numeric | Nick |
|---|---|---|---|---|
| `ACAAK` | OpTwo | | `ACAAP` | VoiceOne |
| `ACAAL` | OpVoice | | `ACAAQ` | PlainOne |
| `ACAAM` | OpOne | | `ACAAR` | HidTwo |
| `ACAAN` | HidThree | | `ACAAS` | HidOne |
| `ACAAO` | PlainTwo | | | |

What it covers:

- `SERVER ... J11`, the `CAP` line, and `N` lines carrying `@time=` message tags.
- `#p11-bans`: three ban triples (`mask timestamp setter`) from two setters, and
  the mode block `+tnlk 25 sekrit` (limit before key).
- `#p11-plain`: status-less members, `:v`, `:o`, and op plus voice sent as `:vo`.
- `#p11-deljoin`: a `:d` group of two (HidTwo, HidOne) on a `+D` channel.
  PlainOne had joined hidden and then spoken, and is burst as a plain member:
  the hub learned of that through REVEAL.
- `#p11-wasdel`: a `:d` group (HidThree) on a channel that is no longer `+D`.
- After the burst, `ACAAS RV #p11-deljoin <ts>`: HidOne speaks and is revealed
  while gnuworld is linked.
