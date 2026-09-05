# epoll-game-server

A single-threaded, event-driven TCP server built on Linux `epoll`, it is to be the network foundation of a small multiplayer game. 
It's written in C++17 with no dependencies beyond the standard library and POSIX.

**Status: in progress.** The connection lifecycle, per-IP admission control, and logging are
implemented. 
See [Not implemented yet](#not-implemented-yet) for what remains.

This is entirely a learning project. I am a second-year CS undergraduate and this is my first major project and also the largest piece of systems code I have written. [`DEVLOG.md`](DEVLOG.md) records what I built, what broke, what was understood and what I managed to change my mind about with appropriate dates.

---

## Building

Requires a Linux system (uses `epoll`), CMake 3.16+, and a C++17 compiler.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
./build/server
```

The server listens on port 5555 (IPv4 only). Configure that with `include/proto/settings.h`.

For a sanitizer build:

```sh
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
cmake --build build-asan
```

---

## Layout

```
include/core/     logging, clock, formatting helpers, threadpool
include/proto/    wire format, parser, network settings
include/server/   connection struct, server class, tunings, socket setup
src/              implementations
```

---

## Design decisions

Interestingly, many of the decisions below were reached after I got something wrong first...

### Nothing gets destroyed outside `reap()`

Every event (apart from the listener socket) in a batch returned by `epoll_wait` has a non-owning `connection *` to the corresponding connection object. This is stored in `event.data.ptr`.

This means that at the instance a connection is to be dropped (due to some error or semantic invalidity), if events are able to drop connections in the middle of the batch, then the pointers to these connection objects for the remaining events in the batch are potentially unsafe to dereference or could be stale.

This is because the server mechanism does (will) involve a client being able to close another client's connection due to a send failure. Thus, the `alive` dereference at the top of the processing loop for a client whose connection has already been dropped is UB.

To avoid this and to unify the process of deleting a client, connection deletion is deferred to after the event batch has been fully processed via `reap()`. Connection objects held in an `unordered_map<uint64_t,std::unique_ptr<connection>>` are all marked dead via a single method `markdead()`.

`markdead()` only ever sets `alive = false` and adds to a `toremove` vector and never deletes. This paired with `!=alive` idempotency test, allows the invariant that **every pointer within `processevents()` is NEVER stale.** And this fact allowed me to change my structure and letting methods in `processevents()` to take raw `connection` pointers directly instead of an id (which avoided redundant hash lookups).

### IP entries outlive the connections that created them

Essentially, per-IP admission control has two parts:

- **Cap on concurrent connections** (`count`), which bounds the number of simultaneous sockets from one address
- a **token bucket**, which bounds the connection **rate** from one address.

The cap limits how much resource can be occupied, the bucket limits the rate at which an IP can create and destroy connections to make the server work for it.

An `ipmap` entry is not erased when its last connection closes. Otherwise disconnecting would reset the rate limiter and an attacker could connect-disconnect-reconnect to get a full bucket. Removal only happens when `count = 0` **and** also the token bucket for that entry has refilled.

The removal happens at two places. Opportunistically in `markdead()` and also in `reap()` for those clients whose `count` was nonzero and managed to hit their token limit.

### Token refill is lazy

There is no dedicated background timer running constantly per IP. `updatetokens()` uses `lastseen` to evaluate the bucket only when it is actually inspected. (on accept, on disconnect, and during the sweep.)

The IP tokenbucket was one of the first places where I realised to unify changes to fields that need to be changed together. Originally, the `lastseen` field update and the token update were separate in the sense that `updatetokens()` only calculated the new tokens value and the caller had to update `lastseen`. It was poor design since if one call site did not update the `lastseen` field, future token updates would carry stale values. And that is exactly what happened that prompted me to put both updates in a single method

There is a difference between what `tokens` and `count` represent per IP entry. `tokens` represent actual CPU work spent, so it is consumed directly on `accept()` regardless of what happens next, whereas `count` represents actually alive connections for that IP, so it is only incremented after a succesfull `epoll_ctl` ADD. I had gotten that wrong which led to `count` being leaked on failure paths of the ADD call.

### The connection pool is not shared

An IP address is cheap to acquire in bulk. It is abundant identity which a real account is comparatively not. So the entire connection budget is split into two portions.

A small pool for unauthenticated connections and a larger pool for authenticated ones. When the unauth pool fills, new connections are refused even though authenticated slots are free. So anonymous connections cannot displace actually authenticated clients. (But auth timeouts also prevent unauth slots from being always occupied by a fast attacker).

Because the two counters (`unauthconncount` and `authconncount`) must move together on promotion, state transitions are handled by a unified method that increments or decrements globals based on the old and new state. this again prevents subtle bugs in terms of forgetting to update the counters at the state change call sites to appear.

### fixed timeout makes a FIFO sufficient

Unauthenticated connections have a strict deadline of being authenticated or being dropped. This prevents the unauth pool from just being slots that an attacker can occupy all the time. And the paired up token bucket handles the rate issue.

Because the timeout is the same constant for everyone, and connections arrive in order:

```
t1 < t2 < t3   implies   t1 + d < t2 + d < t3 + d
```

So the connections expire in the order that they arrive, if the oldest connection at the front has not expired, then the others behind have not too. That is the reason that I used a plain FIFO structure or the connection ID's.

Promotion from unauthenticated to authenticated for an entry sitting in the middle of the queue requires removal that is O(N) and beats the point. Which is why they are not removed and sit inside the queue even if stale. Once these entries reach the front, they are naturally removed. The cost is acceptable little memory.

The queue stores ids instead of pointers simply because a stale id is **detectable** through `find()` (and pointers are not).

### Logging allocates nothing

No heap allocations on any log path. Logging uses a stack buffer and a `appender` method that helps prevent overflows and instead truncates long messages.

`SERVERASSERT` aborts even in release builds, since breaking invariants is fatal to the server. So they are displayed loud and clear when broken.

---

## Wire format

Length prefixed binary data. All messages have a 4 byte header that describes the message length including a 1 byte type field present on every packet.

Fixed size primitives, `__attribute__((packed))` and proper byte order conversions ensure that the sender and receiver agree on the structure of the packets.

---

## Not implemented yet

- **Authentication.** The setup is ready, not fully done
- **Heartbeats** for authenticated connections.
- **Message-level rate limiting.**
- **Sessions** for reconnection.
- **Matchmaking**, at this point, getting this right is the ultimate goal.
- **Testing and benchmarks.** None right now. Every limit in `serversettings.h` is essentially not measured, and has only been reasoned about. so they are guesses at the moment.

Known-accepted limitations:

- IPv4 only.
- Memory allocation failures are unhandled (an allocation failure ends the process rather than handling/degrading it)
- The defence is only at the application level. attacks that exhaust the resources below this layer are outside the scope of the project.

---

## License

MIT. See [LICENSE](LICENSE).