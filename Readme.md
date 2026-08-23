# epoll-game-server

A single-threaded, event-driven TCP server built on Linux `epoll`, it is to be the network foundation of a small multiplayer game. 
It's written in C++17 with no dependencies beyond the standard library and POSIX.

**Status: in progress.** The connection lifecycle, per-IP admission control, and logging are
implemented. Authentication, the write path, matchmaking, and
tests are not. See [Not implemented yet](#not-implemented-yet) for the honest list.

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
include/core/     logging, clock, formatting helpers
include/proto/    wire format, parser, network settings
include/server/   connection struct, server class, tunings, socket setup
src/              implementations
```

---

## Design decisions

Each of the decisions below was made after getting something wrong first.

### Nothing is destroyed outside `reap()`

`epoll_wait` returns a batch of events, and each event carries a non-owning
`connection*` in `data.ptr`. 
This means that if any handler can destroy a connection mid batch, then every data.ptr of the subsequent events become suspect. Which becomes even more pronounced when a client can delete other clients.

that is why destruction is deferred. Connections live in an `unordered_map<uint64_t, unique_ptr<connection>>`. `markdead()` never erases, it sets `alive = false` and pushes the id into a `toremove` vector. `reap()` runs after a batch is entirely processed and then erases the connectionsmap entries, which is when RAII closes the socket.

The consequence is the invariant: **within a batch, no `connection*` is ever stale.** Handlers can take raw pointers directly instead of ids, and skip a hash lookup on every access, this is because a dead connection (alive = false) is guaranteed to be in memory until the batch is fully processed.

I was originally passing ids everywhere and calling `find()` to validate. While that is safe, it is redundant as it pays a lookup to answer a question that the architecture already answers.

### IP entries outlive the connections that created them

Per IP admission control has two parts (and both are keyed on the raw uint32_t IPV4 address):

- a **concurrency cap** (`count`), which bounds the number of simultaneous sockets from one address
- a **token bucket**, which bounds the connection **rate** from one address.

The two catch different attacks: the cap limits how much resource can be occupied, the bucket limits the rate at which an IP can create and destroy connections to make the server work for it.

The important thing is that an `ipmap` entry is not erased when its last connection closes.
If it were, disconnecting would reset the rate limiter and an attacker could connect-disconnect-reconnect to get a full bucket.

An entry is only removed when `count == 0` **and** the bucket has refilled to full capcity (so now it carries no remaining information). Cleanup happens opportunistically in `markdead()` and then through periodic sweep for those entries that hit the token
limit while still having connections alive.

### Token refill is lazy

No background timer per IP. `updatetokens()` uses `lastseen` to evaluate the bucket only when it is actually inspected.
(on accept, on disconnect, and during the sweep.)

Splitting the change in `lastseen` from the token update was a real bug: with `lastseen` updated by the caller, one
call site forgot, and stale values were used repeatedly. heavy connections *earned* tokens, which defeated the entire point of the bucket. Does updates to both tokens and `lastseen` were unified in the same method.

There is a distinction between what `tokens` and `count` represent per IP entry. tokens represent actual CPU work spent, so it is consumed directly on `accept()` regardless of what happens next, whereas count represents actually alive connections for that IP, so it is only incremented after a succesfull `epoll_ctl` ADD. I had gotten that wrong which led to count being leaked on failure paths of the ADD call.

### The connection pool is not shared

An IP address is cheap to acquire in bulk. It is abundant identity which a real account is comparitively not.
So the entire connection budget is split into two: a small pool for unauthenticated connections and a larger reserved pool for authenticated ones. When the unauth pool fills, new connections are refused even though authenticated slots remain free.

This does not stop a distributed flood, but it allows no number of anonymous connections to displace actually authenticated clients.

Because the two counters (`unauthconncount` and `authconncount`) must move together on promotion, state transitions are handled by a unified method that increments or decrements globals based on the old and new state. this again prevents subtle bugs in terms of forgetting to update the counters at the state change call sites to appear.

### fixed timeout makes a FIFO sufficient

Unauthenticated connections get a deadline: authenticate within a few seconds or be dropped.
This prevents the unauth pool from just being slots that an attacker can occupy indefinitely. so the space problem now becmes a rate problem which the token bucket helps in handling.

Because the timeout is the same constant for everyone, and connections arrive in order:

```
t1 < t2 < t3   implies   t1 + d < t2 + d < t3 + d
```

Thus, the arrival order is the expiry order. So a plain FIFO of ids suffices: inspect the front, and if it
has not expired, nothing behind it has expired. Beats scanning the entire set of connections in every `reap()` cycle.

Promotion from unauthenticated to authenticated for an entry sitting in the middle of the queue requires removal that is O(N) and beats the point. thus they are not removed at all. stale unauth connections (that are no longer unauth) can sit in the queue and naturally get evicted once they reach the front. The cost is little memory and nothing else.

The queue stores ids instead of pointers simply because a stale id is **detectable** through `find()`. 
A stale pointer is not.

### Logging allocates nothing

Log formatting uses a stack buffer paired with an `appender` helper that tracks the write offset
and truncates rather than overflowing. There are no heap allocations on any log path.

`SERVERASSERT` aborts in release builds and also debug. the architecture relies on invariants which when broken quietly can be fatal to how the server functions. Displayed loud and clear when broken.

---

## Wire format

Length-prefixed binary data: a 4-byte big endian length, then a 1-byte message type, then the actual content.
Structs are `__attribute__((packed))` with fixed-width primitive types and byte-order conversions, so that struct layout does not depend on the compiler or the machine.

---

## Not implemented yet

- **Authentication.** The implementation is yet to be written. (credential verification)
- **The write path.** No `EPOLLOUT` handling (partial writes, policy for slow clients etc.)
- **`EPOLLERR` / `EPOLLHUP` handling.**
- **Heartbeats** for authenticated connections. Will be using a doubly linked list to implement an LRU pattern.
- **Message-level rate limiting.**
- **Matchmaking**, at this point, getting this right is the ultimate goal.
- **Testing and benchmarks.** None right now. Every limit in `serversettings.h` is essentially not measured, and has only been reasoned about. so they are guesses at the moment.

Known-accepted limitations:

- IPv4 only.
- Memory allocation failures are unhandled (an allocation failure ends the process rather than handling/degrading it)
- The defence is only at the application level. attacks that exhaust the resources below this layer are outside the scope of the project.

---

## License

MIT. See [LICENSE](LICENSE).