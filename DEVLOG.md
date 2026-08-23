## 1/08/26

Having completed the POLL server, I am finally getting into EPOLL, its linux specific and very powerful. I want to first only make a single threaded server, that can handle itself under pressure. this is my first big project...

I am realising as I study into this that knowing POLL helps alot since much of the mechanism is the same. only that epoll is more efficient.
built a basic structure with `EPOLLIN`, `EPOLLHUP`/`EPOLLERR`, and `EPOLLOUT`.

## 3/08/26

sidetracked a bit to read up about handling incoming messages, and a protocol for transferring packets over the wire. so I can use json, but then there is thing called **binary serialization**.
so essentially I can send structs (which under the hood, is a contiguous block of bytes) over the wire, and the receiving computer can deserialize it.
of course it was not that easy. for example, I learnt that compilers pad struct fields in memory for efficiency, that int can have different default size on different systems and then the fact of byte order being different on different machines.
also that I cannot send dynamic length data easily, for that I would need a length field before the raw data for the other computer to know how much to read.
pointers over the wire become useless as they only talk about some address in memory, at which, what's actually present is different on the sender and receiver and reading this way can cause a segfault.

so I had to address each of these issues. for example using fixed size primitives like `uint32_t` etc, using the compiler packed attribute to avoid compiler padding, and using host to network and vice versa conversions for consistent byte ordering.
will start working on a rough structure of handlemessage, and I have decided to use c style byte buffers.

## 5/08/26

now, each active client in the server is now determined by a connection struct. a major part of the day was spent tackling with issues regarding the lifetime of a client.
so originally, if a fd set the `EPOLLER`/`EPOLLHUP` bit, then it would be dropped inside the loop itself. so, with epoll, an fd gets removed from the instance if it is closed automatically.
so I stored the fd in `event.data.fd` when creating it on `accept()`. and as for the connection objects, they were stored directly in an unordered map that was keyed by fd.
but that introduced an entire new class of issues, like me forgetting to erase the heap allocated structs keyed by an fd that was dead and was assigned to a new client who now pointed towards a connection struct that wasn't theirs.
another problem was considering future code would have involved matchmaking, a client in a batch could possibly delete another client in the batch. but the alive check at the top of the event loop needed access to a connection object that was now dead leading to use after free.

all in all, I researched and came to a better solution. **deferred deletion and RAII combination**.
instead of using explicit `exit()` in every error path, what I can do instead is use RAII compliant structures. manual memory management was dropped.
so now I am using unique pointers that store the connection on the heap first. then I use a connectionsmap that keys by an ever incrementing playerid assigned to each client. it can never repeat. the map stores the unique pointer as values. having unique pointers means that the `event.data.ptr` can store view only raw pointers to the connection object on the heap! but it also means that any connection, before being purged must never have a pending inspection via `event.data.ptr` to check the alive flag for example, otherwise its a dangling pointer. thus within a batch, deleting is fatal.
so,
any connection that runs into some error, always goes through a **unified markdead method** that has an idempotent `if !alive return` check to prevent double insertions into a vector called toremove. markdead sets alive to false after an entire batch of events has been processed, `toreap()` runs that iterates the toremove vector to remove connections from the map.

and the map id's also allows me to always check the existence of a connection object by running `find()` on the map by id, since that is the single source of truth about the connection's aliveness. so during event batch processing, methods have to take id's and do a lookup to know that the connection is still active.

## 9/08/26

over the past few days, I have seen other code, understand concepts and start building. had to learn a bit of cmake since vscode + `tasks.json` is a nightmare.
also about organization of code and include files in relevant directories.
one of the most important things is building a logging mechanism to actually know what my server is doing. so that was another journey into what macros are (preprocessor copy paste) and how to use them.

it was actually cool getting into the macro structure to have different levels of logging intensity, but the actual building of the system was time intensive.
c style formatting using `snprintf`, `vsnprintf` and maintaining track of variables to avoid writing out of bounds was challenging. had to write, rewrite, rewrite again with the pointer arithmetic.
finally decided to write a helper **APPENDER**, that handles the repetitive checks and ensures that only as much is written as the buffer allows.

added safety checks against ubounded message lengths and a base parser that handles incoming messages by draining the recv buffer.

## 10/08/26

right now, the recv buffers drains and fills the readbuffer, which then evaluates packets one by one and in case of incomplete messages breaks for future read cycles to complete the processing.
but I read a bit about rate limiting, because processing costs my server alot while costing the sender a few syscalls.
tocken bucket rate limiting idea seems interesting...
I realize that the same needs to be applied to IP and who gets to join at what rate, and how many concurrent connections at a time.

## 18/08/26

finally Implemented token bucket for IP rate limiting, it seemed scary at start but got the main idea. had been worrying alot about using the wrong data structures to waste memory or speed. then looked at my scope and scale, and decided to use what I know and what genuinely is suitable. better alternatives might exist (read somewhere about timing wheels, min heap etc.), but sticking with a hashmap.

learnt about `try_emplace` in an unordered map. using `[it,inserted]`, it automatically sets it if the entry exists and if inserted, we can set it to the new value.
this helped double lookups in the old pattern where I was using `.find()` to check if the pair existed, doing some similar changes if it did (increment count and use token), and otherwise using it to make changes.

setting values for the token capacity, max concurrent users etc. was like shooting in the dark because I genuinely had no idea what to set that would make the server not be too restrictive but also not too less so that it can backfire. had to do a bit of digging for that and settled for some modest values.

## 19/08/26

one of the assertions in markdead is that an IP entry in ipmap must exist if markdead is called and connection is alive. but that was not true in only one case, that is when epoll ctl fails. once that happens, the connection is not added to the connectionsmap. and ip count was being incremented after the rate and concurrent counts checked passed before the epoll ctl call. so if it failed, that count and entry would leak. so **moved ipcount increment post ctl call success**.

another important distinction I learned is that tokens are a result of work spent by the cpu, whereas count just tracks the number of active connections from that IP.
thus **token decrement is kept** **before the ctl call** AS in `accept()`, the cpu does work for the incoming connection, regardless of whether ctl fails or not.

token arithmetic does not use a real time background clock that is constantly ticking. it is lazy in the sense that it evaluates the time gap from now to last seen only when the tokenbucket is looked upon or evaluated.
above **exposed a bug** in my code. tokens were updated on accept, but they were never updated on markdead when the tokencount was inspected. this meant that a single client could connect from some IP (costing a token), spend x hours doing something, then disconnect. markdead would still see their tokens at 14 since they are not updated in the background constantly. but the IP entry should have been cleaned due to 0 count and sufficient time for tokens to have refilled.

**thus markdead should also update tokens and then check!**.

`reap()` does two jobs, clearing dead connections using toremove ids, and also an O(N) scan over the ipmap to clean IP entries based on the 0 and full bucket condition.
now epoll wait needs a timeout otherwise reap becomes event dependent. **1 second seems fine.**
also, the O(N) scan of ipmap after every batch has been processed is a bit much based on the need. rather do it after every 4-5 second interval.

**ANOTHER BUG** : `if (currentconn->alive == false) return -1;` this check was happening for each event. but for a listener event, that's like dereferencing a nullptr, so its UB.

## 20/08/26

much of today was about learning ideas and concepts. and thinking up of a plan. one of the defensive mechanisms I am implementing is to partition the max pool of 10k clients into two. the larger portion only accomodates authorized clients, whereas the smaller 2k pool is for unauthorized clients. this is maintained using the unauth and authconncounts.
learnt a good lesson when my code broke and displayed weird data because of the ease in forgetting to update two global counters, **so I decided to unify them using a state changer** that considers the old state and the requested state to increment or decrement suitable globals.

placing auth timeouts for unauth clients, failure to meet the deadline leads to eviction. but the problem was that the 'easy' and known way meant scanning all the connections in `reap()`, checking the time gap and deciding which ones to kick. but this is when I came across the **benefit of having fixed timeouts for all clients.**

considering that each connection arrives sequentially,

```
t1 < t2 < t3
```

and that timeout 'd' is fixed for each,

```
t1 + d < t2 + d < t3 + d
```

this means that **the oldest unauthorized connection is the first one to timeout if IT DOES!**. so if all 3 timed out, the first one would be the first to time out.
which means I can just use a queue. inspect the front, if they have not timed out, the others have not too (this works because unauth -> auth timeout window promotion is a one time thing, we do not check the same client twice for it unlike heartbeat packets). if they have timed out, pop them out and inspect the next!

But I found out that this poses another problem: if a newer client sitting in the queue gets authorized, we need to remove them. but that would take O(N) to remove from the middle.
so to solve that problem, **I will use the concept of lazy eviction!**.
when a client authorizes, we do not remove them from the queue, we just change the state and the global counts. in reap, eventually when that client is reached, it sees that the state is auth and simple drops them.
I had my fears of this causing memory overhead by keeping stale clients in the queue, but it is very small. and with the IP limits and `reap()` happening every 500-1000ms, they get cleaned up quick and the queue does not fill up too fast (because a queue entry is event driven based on `accept()`)! immense satisfaction!

I realise this will not work for heartbeat packets though...will have to see about that, something about doubly linked lists etc. that I am not touching right now...

## 21/08/26

One of the consequences of having deferred deletion is the absolute invariant that during processing of a batch of events in the event loop, every single non listener (or connection) `data.ptr` is NOT stale.
this is due to the fact that the connection objects on the heap are only ever removed in `toreap()`, markdead never deletes and only sets alive = false.
so accessing the alive field of the connection object to stop further processing if not alive allows every method in processevents to directly take a raw pointer to the connection object as its parameter. this saves redundant lookups inside connectionsmap since by design, they are guaranteed to be present. so the invariant and the alive checks are doing a lot of heavy lifting.

markdead also follows above principle, every markdead call within processevents or even in `toreap()` is **BEFORE** the actual toremove iteration removal.
read up on the doubly linked list implementation for inactive auth client cleanup, will be implementing it next...