# GNUWorld Event System

Core tells a module about the network through **events**: 19 network events
and 6 channel events, each delivered by calling one named virtual of
`xClient`, with typed parameters — `OnQuit(iClient*, std::string_view)`,
`OnJoin(Channel*, iClient*, ChannelUser*)`, and so on. A module overloads the
ones it cares about; every other one has an empty default and costs nothing.
There is no generic handler and no untyped payload: what used to be one
`OnEvent(const eventType&, void*, void*, void*, void*)` switched on by hand
is gone, and with it the four-void* channel counterpart. See
[Migration Guide](#migration-guide) if that is what your module still does.

## Table of Contents

- [Concepts](#concepts)
- [Guarantees](#guarantees)
- [Registering for Events](#registering-for-events)
- [The Full Event List](#the-full-event-list)
- [Delivered Alongside Channel Events, but Not in the Enum](#delivered-alongside-channel-events-but-not-in-the-enum)
- [Migration Guide](#migration-guide)
- [Worked Example](#worked-example)
- [Adding a New Event](#adding-a-new-event)

## Concepts

A **network event** is about the network as a whole: a client opering up, a
quit, a G-line, a server joining or leaving. A module hears one by calling
`MyUplink->RegisterEvent(EVT_QUIT, this)` — one call per event it wants,
naming the event by its `NetworkEvent` enumerator (`include/events.h`).

A **channel event** is about one channel: a join, a part, a topic change, a
mode. Registration is **per channel, not per event**:
`MyUplink->RegisterChannelEvent(chanName, this)` subscribes a module to
*every* channel event on that channel — there is no way to ask for only
joins. `xServer::CHANNEL_ALL` is the string `"*"`, a channel name meaning every
channel that exists on the network: one call,
`RegisterChannelEvent(CHANNEL_ALL, this)` (or the literal `"*"` — they are
the same string), and every channel event on every channel is delivered to
it from then on. A listener registered for `CHANNEL_ALL` is called before
one registered for the specific channel the event is on.

`UnRegisterEvent`/`UnRegisterChannelEvent` undo a registration; registering
twice for the same `<event, client>` or `<channel, client>` pair is
harmless — a client is only ever in a listener list once.

## Guarantees

These are established once, in `src/server_events.cc`, and hold for every
event, network or channel:

- **Every subscriber sees every event in the order things happened.** A post
  made while nothing is being dispatched runs immediately; a post made from
  *inside* a handler — because posting one event caused another — is queued
  instead of dispatched re-entrantly, and drained, oldest first, once the
  outermost dispatch returns. A handler can never see event B before event A
  when A happened first, even if B was posted from deeper in the call stack.
- **Posting from inside a handler never recurses into module code.** It
  queues (`pendingEvents`) and returns; the queued dispatch runs after the
  handler that posted it, and after anything already ahead of it in the
  queue.
- **An object stays alive until the dispatch that is delivering it
  finishes, even if a handler removes it from the network.** Core deletes an
  `iClient`, `iServer`, `Channel`, `ChannelUser` or `Gline` through one
  internal `destroy()` and nowhere else: while a dispatch is in progress it
  holds the deletion instead of doing it, and runs every held deletion once
  the outermost dispatch has returned (`releaseHeldObjects()`, once per line
  the main loop processes). A pointer a handler was handed is safe to read
  for the rest of that dispatch, whatever another handler does to it.
- **The network tables update immediately; only the notification and the
  memory reclaim are deferred.** For most events the post happens before the
  object is removed from `Network`'s tables — the per-event note in the
  table below says so where it matters (`OnQuit`, `OnKill`: "still fully
  attached, posted before it is removed") — so a handler that looks the
  object up again mid-dispatch still finds it. Once core does remove an
  object, that removal is not queued or delayed by anything above; a handler
  running afterward will not find it by looking it up again, even though the
  pointer it was directly handed is still safe to read.

**A handler may** post further events, `Kick`/`Mode`/`Part`/etc. on the
network, and register or unregister any client for anything — the walk that
calls handlers re-reads the listener list before every call, so a client
that unregisters mid-dispatch is not called again, and one that registers is
not called for the event already in progress.

**A handler must not** assume that an object it was handed is still on the
network, still on a channel, or still findable through `Network`/`Channel`
lookups once *it itself* has acted on the network (kicked it, parted a
channel out from under it, etc.) — the pointer stays valid to read, but its
state may already reflect the action. Look the object up again rather than
assuming what you last saw of it still holds.

## Registering for Events

```cpp
MyUplink->RegisterEvent(EVT_QUIT, this);          // one network event
MyUplink->RegisterChannelEvent("#channel", this); // every channel event, one channel
MyUplink->RegisterChannelEvent(xServer::CHANNEL_ALL, this); // every channel event, every channel
```

`RegisterEvent` only accepts a `NetworkEvent` value (`validEvent()` bounds it
to `[0, EVT_NOOP)`, `EVT_NOOP` being where the channel enum starts) — there
is no equivalent bounds check to pass a channel event by number, because
`RegisterChannelEvent` never takes one.

## The Full Event List

Generated from the named virtuals of `xClient` (`include/client.h`) that
core posts through a typed `post*` function of `xServer`
(`include/server.h`, implemented in `src/server_events.cc`) — one row per
`NetworkEvent`/`ChannelEvent` enumerator that something actually posts.

### Network events — `RegisterEvent(EVT_*, this)`

| Virtual | Fires when |
|---|---|
| `OnOper(iClient* theClient)` | theClient has been given +o |
| `OnNetBreak(iServer* theServer, const iServer* uplink, std::string_view reason)` | theServer has left the network, alone or as one of a split; `uplink` is the server it broke from, null for a leaf of a split whose root is already out of the tables |
| `OnNetJoin(iServer* theServer, const iServer* uplink)` | theServer has joined the network, or a jupe of one has been added; `uplink` may be null — only an inbound SERVER names it |
| `OnBurstComplete(iServer* theServer)` | theServer has finished its net burst; for our own uplink this is also where our burst ends |
| `OnBurstAck(iServer* theServer)` | theServer has acknowledged the end of a burst (EA) |
| `OnEndOfBurstAckSent(iServer* theServer)` | we have written our own end-of-burst acknowledgement to theServer |
| `OnGline(Gline* theGline)` | a G-line has been set, by the network or by one of our own clients |
| `OnRemGline(Gline* theGline)` | a G-line has been removed, or has expired |
| `OnQuit(iClient* theClient, std::string_view reason)` | theClient has quit, is going with the server it was on, or is one of ours being detached. `reason` may be empty. **Attachment differs by cause**: a client that quit is still fully attached, because the event is posted before it is removed; a client going with its server is not — `xNetwork::removeServer()` removes it first and posts afterwards, so the object is live but the network no longer knows it, nor the server it was on |
| `OnKill(const NetworkTarget* source, iClient* theClient, std::string_view reason)` | theClient has been killed, still fully attached; `source` is the client or server that did it, null when one of our own modules did (`xClient::Kill()`) |
| `OnNick(iClient* theClient)` | a client has appeared on the network, or a module has spawned a fake one |
| `OnNickChange(iClient* theClient, std::string_view oldNick)` | theClient has changed nick; it already answers to the new one |
| `OnAccount(iClient* theClient)` | theClient has logged in to an account |
| `OnAccountFlags(iClient* theClient)` | the account flags of theClient, already logged in, have changed |
| `OnRaw(std::string_view line)` | one line read from the uplink, after it has been handled |
| `OnXQuery(iServer* theServer, std::string_view routing, std::string_view message)` | an inter-service query from theServer, to be answered with an XR |
| `OnXReply(iServer* theServer, std::string_view routing, std::string_view message)` | the answer to one of those |
| `OnNetConf(iServer* theServer, std::string_view key)` | a network configuration variable has been set by theServer |
| `OnRemNetConf(iServer* theServer, std::string_view key)` | a network configuration variable has been removed by theServer |

`NetworkEvent` has two more enumerators, `EVT_JUPE` and `EVT_UNJUPE` —
nothing posts either (a jupe is folded into `OnNetJoin`); there is no virtual
for them, and registering for them has no effect.

### Channel events — `RegisterChannelEvent(chanName, this)`

| Virtual | Fires when |
|---|---|
| `OnJoin(Channel* theChan, iClient* theClient, ChannelUser* theUser)` | theClient has joined theChan, which already existed |
| `OnBurstJoin(Channel* theChan, iClient* theClient, ChannelUser* theUser)` | the same, for a membership that arrives in a net burst |
| `OnCreate(Channel* theChan, iClient* theClient, ChannelUser* theUser)` | theClient has created theChan, and is opped in it |
| `OnPart(Channel* theChan, iClient* theClient, std::string_view message)` | theClient has left theChan, and is already off it. `message` may be empty — only a PART from the network carries one |
| `OnTopic(Channel* theChan, iClient* theClient, std::string_view topic)` | the topic of theChan has been set; theClient is null when a server set it, as one arriving in a burst is |
| `OnServerMode(Channel* theChan, iServer* theServer)` | theServer has changed the modes of theChan; the changes themselves arrive through `OnChannelMode()` and its kin — see below |

That is 19 + 6 = 25 events, each with its own typed `post*` function on
`xServer` (`postOper`, `postQuit`, `postJoin`, …) — core calls `post*()`,
`post*()` calls the same-named virtual on every listener.

## Delivered Alongside Channel Events, but Not in the Enum

A channel registration also delivers two things that are **not**
`ChannelEvent` enumerators, have no `EVT_*` value, no `eventName()`, and
predate this event system rather than having gone through the migration
below — they were always typed:

- **`OnNetworkKick(Channel* theChan, iClient* srcClient, iClient* destClient, const std::string& kickMessage, bool authoritative)`**
  — a kick on theChan. `srcClient` may be null (the ircu protocol allows a
  server to KICK). `authoritative` is false while `destClient` is still on
  the channel, a ZOMBIE pending its own server's PART.
- **The eight `OnChannelMode*` virtuals** (`OnChannelMode`, `OnChannelModeL`
  /`K`/`A`/`U`/`O`/`V`/`B`, for the simple, limit, key, Apass, Upass, op,
  voice and ban modes respectively) — each invoked once per batch of that
  kind of change in one line, `sourceUser` null when a server set the mode.

Both are delivered to the same listener list a channel event is
(`RegisterChannelEvent`); there is no way to register for channel events but
not these, or for these but not channel events.

## Migration Guide

For a module still built against the untyped API: `xClient::OnEvent(const
eventType&, void* Data1, void* Data2, void* Data3, void* Data4)` and
`xClient::OnChannelEvent(const channelEventType&, Channel*, void* Data1,
void* Data2, void* Data3, void* Data4)`, and posting through
`xServer::PostEvent()`/`PostChannelEvent()` (an event number plus four
`void*`). **All four of these are gone.** A module that still defines
`OnEvent`/`OnChannelEvent`, or calls `PostEvent`/`PostChannelEvent`, no
longer compiles.

### The mechanical translation

- A `case EVT_FOO:` in your `OnEvent`/`OnChannelEvent` switch becomes an
  override of the named virtual for that event, from the tables above.
- Each `void*` slot becomes the corresponding named, typed parameter — cast
  and all: no more `static_cast<iClient*>(data1)`.
- A call to `MyUplink->PostEvent(EVT_FOO, ...)` /
  `PostChannelEvent(EVT_FOO, ...)` (a module posting an event itself, not
  just receiving one) becomes a call to the matching `post*()` — e.g.
  `PostEvent(EVT_ACCOUNT_FLAGS, static_cast<void*>(theClient), 0, 0, 0,
  this)` becomes `postAccountFlags(theClient, this)`. The exclude parameter
  some `post*` functions take (`postGline`, `postRemGline`, `postAccount`,
  `postAccountFlags`) is the same "one client not told" argument
  `PostEvent` took as its last one.

### Slot-to-parameter mapping

`Channel*` is already a separate argument on every `OnChannelEvent`
override, not one of the four slots — only `data1..data4` are mapped below.

| Event | Old `void*` slots | New parameters |
|---|---|---|
| `EVT_OPER` | `data1` = `iClient*` | `OnOper(iClient*)` |
| `EVT_NETBREAK` | `data1` = `iServer*` theServer, `data2` = uplink (inconsistently typed — sometimes `iServer*`, sometimes a raw string on a cascading squit), `data3` = reason string | `OnNetBreak(iServer*, const iServer* uplink, std::string_view reason)` — `uplink` is now always a typed pointer, null when not known |
| `EVT_NETJOIN` | `data1` = `iServer*` theServer, `data2` = uplink, typed only at some of its three posting sites | `OnNetJoin(iServer*, const iServer* uplink)` — `uplink` always a typed pointer, null when a SERVER line named none |
| `EVT_BURST_CMPLT` | `data1` = `iServer*` | `OnBurstComplete(iServer*)` |
| `EVT_BURST_ACK` | `data1` = `iServer*` | `OnBurstAck(iServer*)` |
| `EVT_EA_SENT` | `data1` = `iServer*` | `OnEndOfBurstAckSent(iServer*)` |
| `EVT_GLINE` | `data1` = `Gline*` | `OnGline(Gline*)` |
| `EVT_REMGLINE` | `data1` = `Gline*` | `OnRemGline(Gline*)` |
| `EVT_QUIT` | `data1` = `iClient*` theClient, **`data2` = reason string** | `OnQuit(iClient*, std::string_view reason)` |
| `EVT_KILL` | `data1` = source (`iClient*` or `iServer*`, untyped), **`data2` = `iClient*` theClient**, `data3` = reason string | `OnKill(const NetworkTarget* source, iClient* theClient, std::string_view reason)` |
| `EVT_NICK` | `data1` = `iClient*` | `OnNick(iClient*)` |
| `EVT_CHNICK` | `data1` = `iClient*` theClient, `data2` = old nick string | `OnNickChange(iClient*, std::string_view oldNick)` |
| `EVT_ACCOUNT` | `data1` = `iClient*` | `OnAccount(iClient*)` |
| `EVT_ACCOUNT_FLAGS` | `data1` = `iClient*` | `OnAccountFlags(iClient*)` |
| `EVT_RAW` | `data1` = line string | `OnRaw(std::string_view line)` |
| `EVT_XQUERY` | `data1` = `iServer*`, `data2` = routing chars, `data3` = message chars | `OnXQuery(iServer*, std::string_view routing, std::string_view message)` |
| `EVT_XREPLY` | `data1` = `iServer*`, `data2` = routing chars, `data3` = message chars | `OnXReply(iServer*, std::string_view routing, std::string_view message)` |
| `EVT_NETCONF` | `data1` = `iServer*`, `data2` = key string | `OnNetConf(iServer*, std::string_view key)` |
| `EVT_REMNETCONF` | `data1` = `iServer*`, `data2` = key string | `OnRemNetConf(iServer*, std::string_view key)` |
| `EVT_JOIN` / `EVT_BURST` / `EVT_CREATE` | `data1` = `iClient*` theClient, `data2` = `ChannelUser*` theUser | `OnJoin`/`OnBurstJoin`/`OnCreate(Channel*, iClient*, ChannelUser*)` — three events, one shared shape; see [fall-through groups](#a-fall-through-group) |
| `EVT_PART` | `data1` = `iClient*` theClient, **`data2` = message string** | `OnPart(Channel*, iClient*, std::string_view message)` |
| `EVT_TOPIC` | `data1` = `iClient*` theClient (null for a burst), **`data2` = topic string** | `OnTopic(Channel*, iClient*, std::string_view topic)` |
| `EVT_SERVERMODE` | `data1` = `iServer*` | `OnServerMode(Channel*, iServer*)` |

**The slot that changed meaning: `data2`.** For `EVT_QUIT` it was the reason
text; for `EVT_KILL` it was the `iClient*` being killed (the reason moved to
`data3`); for `EVT_PART` and `EVT_TOPIC` it was a string again (the part
message, the new topic). A handler that read `data2` as one type for one
event and copied the same cast for another was exactly the bug this
migration removes — the new parameters are named and typed per event, so
there is nothing left to get backwards.

`EVT_KICK` is not in this table: `OnNetworkKick` was already a dedicated,
typed method before this migration (see
[above](#delivered-alongside-channel-events-but-not-in-the-enum)) — there
was never a `void*` form of it to convert.

### A fall-through group

`EVT_BURST`, `EVT_CREATE` and `EVT_JOIN` are, to most modules, the same
thing — a membership showing up — with only a flag distinguishing a burst.
The old code expressed that as a `switch` fall-through:

```cpp
switch (whichEvent) {
case EVT_BURST:
    burstJoin = true;
    // fall through
case EVT_CREATE:
case EVT_JOIN: {
    theClient = static_cast<iClient*>(data1);
    // ... one body, shared by all three ...
}
}
```

There is no fall-through between virtuals. The replacement is one
one-line override per event, each calling a shared helper with the right
flag (real code, `mod.cservice`):

```cpp
void cservice::OnBurstJoin(Channel* theChan, iClient* theClient, ChannelUser*) {
    handleChannelJoin(theChan, theClient, true);
}
void cservice::OnCreate(Channel* theChan, iClient* theClient, ChannelUser*) {
    handleChannelJoin(theChan, theClient, false);
}
void cservice::OnJoin(Channel* theChan, iClient* theClient, ChannelUser*) {
    handleChannelJoin(theChan, theClient, false);
}
```

### The name-hiding trap

`xClient` declares `OnJoin`/`OnPart`/`OnKill` more than once, for unrelated
purposes:

| Name | Purpose |
|---|---|
| `OnJoin(Channel*, iClient*, ChannelUser*)` | the channel event, above |
| `OnJoin(Channel*)`, `OnJoin(const std::string&)` | **the bot itself** has joined a channel (called after `Join()`) |
| `OnPart(Channel*, iClient*, std::string_view)` | the channel event, above |
| `OnPart(Channel*)`, `OnPart(const std::string&)` | **the bot itself** has parted a channel |
| `OnKill(const NetworkTarget*, iClient*, std::string_view)` | the network event, above |
| `OnKill()` | **the bot itself** has been killed |

In C++, declaring an override of one overload of a name in a derived class
hides every base-class overload of that name in that derived class's own
scope, unless a `using` brings them back. Declare only
`OnJoin(Channel*, iClient*, ChannelUser*)` in your module class, and an
unqualified call to `OnJoin(someChan)` from *inside that class* no longer
compiles — the single-argument overload is hidden, not gone.

**In this codebase it was needed nowhere, across nine converted modules.**
The reason: core always calls these methods through a pointer statically
typed as `xClient*` (or `iClient*`), never through the derived module's own
type, so hiding — which only affects name lookup performed in the derived
class's own scope — never applies to core's calls; virtual dispatch still
finds the right override regardless. And no converted module calls
`OnJoin`/`OnPart`/`OnKill` on itself, unqualified, from its own code — the
self-notification overloads (`OnJoin(Channel*)` and kin) exist to be
*overridden*, not called, and nothing in this tree does both to the same
name in the same class. `mod.cservice` overrides both
`OnJoin(Channel*, iClient*, ChannelUser*)` (the event) and
`OnJoin(const std::string&)` (the bot's own join) — perfectly fine, since
declaring both itself means neither is hidden from the other.

Add `using xClient::OnJoin;` (and the equivalent for `OnPart`/`OnKill`) only
if your module overrides one overload of the name and separately calls
another, unqualified, from inside the same class. Check your own class
for that combination rather than adding the `using` line by habit.

### What no longer exists

- `xClient::OnEvent()`, `xClient::OnChannelEvent()` — deleted, not
  deprecated. A module that still defines either fails to compile.
- `xServer::PostEvent()`, `xServer::PostChannelEvent()` — posting an event
  by number, with four `void*`, is gone; call the named `post*()` instead.
- The `void* Data1..Data4` payload entirely — every event's data now has a
  name and a type at the call site.

## Worked Example

`mod.scanner`, one event handler, before and after (`mod.scanner/scanner.h`,
`mod.scanner/scanner.cc`):

```cpp
// before — scanner.h
virtual void OnEvent(const eventType&, void* = 0, void* = 0, void* = 0, void* = 0);

// before — scanner.cc
void scanner::OnAttach() {
    xClient::OnAttach();
    MyUplink->RegisterEvent(EVT_NICK, this);
}

void scanner::OnEvent(const eventType& whichEvent, void* arg1, void* arg2, void* arg3, void* arg4) {
    switch (whichEvent) {
    case EVT_NICK:
        handleNewClient(static_cast<iClient*>(arg1));
        break;
    case EVT_BURST_CMPLT:
    case EVT_BURST_ACK:
        // Delivered to all clients
        break;
    default:
        elog << "scanner::OnEvent> Received unknown event: " << whichEvent << std::endl;
        break;
    }
    xClient::OnEvent(whichEvent, arg1, arg2, arg3, arg4);
}
```

```cpp
// after — scanner.h
virtual void OnNick(iClient*) override;

// after — scanner.cc
void scanner::OnAttach() {
    xClient::OnAttach();
    MyUplink->RegisterEvent(EVT_NICK, this);
}

void scanner::OnNick(iClient* newClient) { handleNewClient(newClient); }
```

Registration is unchanged. The `switch`, the cast, the two events this
module never acted on (their cases did nothing before, so they simply have
no override now), the "unknown event" fallback (there is no such thing any
more — an event this module does not override is simply never called), and
the forwarding call to the base class are all gone with the untyped method.

## Adding a New Event

1. Add the enumerator to `NetworkEvent` or `ChannelEvent` in
   `include/events.h`, before the count constant that follows it
   (`networkEventCount`/`channelEventCount` are derived from where the enum
   ends, so this is enough to size everything that depends on the count).
2. Add its `case` to the matching `eventName()` switch, right below. Each
   switch has no `default`, on purpose: a new enumerator with no case is a
   `-Wswitch` **warning** (from `-Wall`, `configure.ac`'s default
   `CXXFLAGS`), not a compile error — the build has no global `-Werror`.
   What actually stops it from reaching a release is the project's
   zero-warning build gate, not the compiler refusing to build.
3. Declare the typed `post*()` function on `xServer`
   (`include/server.h`), next to its siblings.
4. Implement it in `src/server_events.cc`: a `notifyEvent()` (network) or
   `notifyChannel()` (channel) call whose lambda invokes the new virtual by
   name on each listener — copy the shape of any neighboring `post*()`.
5. Declare the named virtual on `xClient` (`include/client.h`), with a
   `///` comment saying what it means and when it fires — that comment is
   what the table above is built from.
6. Give it an empty default body in `src/client.cc`
   (`void xClient::OnFoo(...) {}`).
7. Call the new `post*()` from wherever core or `libircu` learns of the
   condition.
