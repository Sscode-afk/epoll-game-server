## 1/08/26

Having completed the POLL server, I am finally getting into EPOLL, its linux specific and very powerful. I want to first only make a single threaded server, that can handle itself under pressure. this is my first big project...

I am realising as I study into this that knowing POLL helps alot since much of the mechanism is the same. only that epoll is more efficient.
built a basic structure with EPOLLIN, EPOLLHUP/EPOLLERR, and EPOLLOUT.

## 3/08/26

sidetracked a bit to read up about handling incoming messages, and a protocol for transferring packets over the wire. so I can use json, but then there is thing called binary serialization.
so essentially I can send structs (which under the hood, is a contiguous block of bytes) over the wire, and the receiving computer can deserialize it.
of course it was not that easy. for example, I learnt that compilers pad struct fields in memory for efficiency, that int can have different default size on different systems and then the fact of byte order being different on different machines.
also that I cannot send dynamic length data easily, for that I would need a length field before the raw data for the other computer to know how much to read.
pointers over the wire become useless as they only talk about some address in memory, at which, what's actually present is different on the sender and receiver and reading this way can cause a segfault.

so I had to address each of these issues. for example using fixed size primitives like uint32_t etc, using the compiler packed attribute to avoid compiler padding, and using host to network and vice versa conversions for consistent byte ordering.
will start working on a rough structure of handlemessage, and I have decided to use c style byte buffers.

## 5/08/26

now, each active client in the server is now determined by a connection struct. a major part of the day was spent tackling with issues regarding the lifetime of a client.
so originally, if a fd set the EPOLLER/EPOLLHUP bit, then it would be dropped inside the loop itself. so, with epoll, an fd gets removed from the instance if it is closed automatically.
so I stored the fd in event.data.fd when creating it on accept(). and as for the connection objects, they were stored directly in an unordered map that was keyed by fd.
but that introduced an entire new class of issues, like me forgetting to erase the heap allocated structs keyed by an fd that was dead and was assigned to a new client who now pointed towards a connection struct that wasn't theirs.
another problem was considering future code would have involved matchmaking, a client in a batch could possibly delete another client in the batch. but the alive check at the top of the event loop needed access to a connection object that was now dead leading to use after free.

all in all, I researched and came to a better solution. deferred deletion and RAII combination.
instead of using explicit exit() in every error path, what I can do instead is use RAII compliant structures. manual memory management was dropped.
so now I am using unique pointers that store the connection on the heap first. then I use a connectionsmap that keys by an ever incrementing playerid assigned to each client. it can never repeat. the map stores the unique pointer as values. having unique pointers means that the event.data.ptr can store view only raw pointers to the connection object on the heap! but it also means that any connection, before being purged must never have a pending inspection via event.data.ptr to check the alive flag for example, otherwise its a dangling pointer. thus within a batch, deleting is fatal.
so,
any connection that runs into some error, always goes through a unified markdead method that has an idempotent if !alive return check to prevent double insertions into a vector called toremove. markdead sets alive to false after an entire batch of events has been processed, toreap() runs that iterates the toremove vector to remove connections from the map.

and the map id's also allows me to always check the existence of a connection object by running find() on the map by id, since that is the single source of truth about the connection's aliveness. so during event batch processing, methods have to take id's and do a lookup to know that the connection is still active.

## 9/08/26

over the past few days, I have seen other code, understand concepts and start building. had to learn a bit of cmake since vscode + tasks.json is a nightmare.
also about organization of code and include files in relevant directories.
one of the most important things is building a logging mechanism to actually know what my server is doing. so that was another journey into what macros are (preprocessor copy paste) and how to use them.

it was actually cool getting into the macro structure to have different levels of logging intensity, but the actual building of the system was time intensive.
c style formatting using snprintf, vsnprintf and maintaining track of variables to avoid writing out of bounds was challenging. had to write, rewrite, rewrite again with the pointer arithmetic.
finally decided to write a helper APPENDER, that handles the repetitive checks and ensures that only as much is written as the buffer allows.

added safety checks against ubounded message lengths and a base parser that handles incoming messages by draining the recv buffer.

## 10/08/26

right now, the recv buffers drains and fills the readbuffer, which then evaluates packets one by one and in case of incomplete messages breaks for future read cycles to complete the processing.
but I read a bit about rate limiting, because processing costs my server alot while costing the sender a few syscalls.
tocken bucket rate limiting idea seems interesting...
I realize that the same needs to be applied to IP and who gets to join at what rate, and how many concurrent connections at a time.

## 18/08/26

finally Implemented token bucket for IP rate limiting, it seemed scary at start but got the main idea. had been worrying alot about using the wrong data structures to waste memory or speed. then looked at my scope and scale, and decided to use what I know and what genuinely is suitable. better alternatives might exist (read somewhere about timing wheels, min heap etc.), but sticking with a hashmap.

learnt about try_emplace in an unordered map. using [it,inserted], it automatically sets it if the entry exists and if inserted, we can set it to the new value.
this helped double lookups in the old pattern where I was using .find() to check if the pair existed, doing some similar changes if it did (increment count and use token), and otherwise using it to make changes.

setting values for the token capacity, max concurrent users etc. was like shooting in the dark because I genuinely had no idea what to set that would make the server not be too restrictive but also not too less so that it can backfire. had to do a bit of digging for that and settled for some modest values.

## 19/08/26

one of the assertions in markdead is that an IP entry in ipmap must exist if markdead is called and connection is alive. but that was not true in only one case, that is when epoll ctl fails. once that happens, the connection is not added to the connectionsmap. and ip count was being incremented after the rate and concurrent counts checked passed before the epoll ctl call. so if it failed, that count and entry would leak. so moved ipcount increment post ctl call success.

another important distinction I learned is that tokens are a result of work spent by the cpu, whereas count just tracks the number of active connections from that IP.
thus token decrement is kept before the ctl call AS in accept(), the cpu does work for the incoming connection, regardless of whether ctl fails or not.

token arithmetic does not use a real time background clock that is constantly ticking. it is lazy in the sense that it evaluates the time gap from now to last seen only when the tokenbucket is looked upon or evaluated.
above exposed a bug in my code. tokens were updated on accept, but they were never updated on markdead when the tokencount was inspected. this meant that a single client could connect from some IP (costing a token), spend x hours doing something, then disconnect. markdead would still see their tokens at 14 since they are not updated in the background constantly. but the IP entry should have been cleaned due to 0 count and sufficient time for tokens to have refilled.

thus markdead should also update tokens and then check!.

reap() does two jobs, clearing dead connections using toremove ids, and also an O(N) scan over the ipmap to clean IP entries based on the 0 and full bucket condition.
now epoll wait needs a timeout otherwise reap becomes event dependent. 1 second seems fine.
also, the O(N) scan of ipmap after every batch has been processed is a bit much based on the need. rather do it after every 4-5 second interval.

ANOTHER BUG : if (currentconn->alive == false) return -1; this check was happening for each event. but for a listener event, that's like dereferencing a nullptr, so its UB.

## 20/08/26

much of today was about learning ideas and concepts. and thinking up of a plan. one of the defensive mechanisms I am implementing is to partition the max pool of 10k clients into two. the larger portion only accomodates authorized clients, whereas the smaller 2k pool is for unauthorized clients. this is maintained using the unauth and authconncounts.
learnt a good lesson when my code broke and displayed weird data because of the ease in forgetting to update two global counters, so I decided to unify them using a state changer that considers the old state and the requested state to increment or decrement suitable globals.

placing auth timeouts for unauth clients, failure to meet the deadline leads to eviction. but the problem was that the 'easy' and known way meant scanning all the connections in reap(), checking the time gap and deciding which ones to kick. but this is when I came across the benefit of having fixed timeouts for all clients.

considering that each connection arrives sequentially,

```
t1 < t2 < t3
```

and that timeout 'd' is fixed for each,

```
t1 + d < t2 + d < t3 + d
```

this means that the oldest unauthorized connection is the first one to timeout if IT DOES!. so if all 3 timed out, the first one would be the first to time out.
which means I can just use a queue. inspect the front, if they have not timed out, the others have not too (this works because unauth -> auth timeout window promotion is a one time thing, we do not check the same client twice for it unlike heartbeat packets). if they have timed out, pop them out and inspect the next!

But I found out that this poses another problem: if a newer client sitting in the queue gets authorized, we need to remove them. but that would take O(N) to remove from the middle.
so to solve that problem, I will use the concept of lazy eviction!.
when a client authorizes, we do not remove them from the queue, we just change the state and the global counts. in reap, eventually when that client is reached, it sees that the state is auth and simple drops them.
I had my fears of this causing memory overhead by keeping stale clients in the queue, but it is very small. and with the IP limits and reap() happening every 500-1000ms, they get cleaned up quick and the queue does not fill up too fast (because a queue entry is event driven based on accept())! immense satisfaction!

I realise this will not work for heartbeat packets though...will have to see about that, something about doubly linked lists etc. that I am not touching right now...

## 21/08/26

One of the consequences of having deferred deletion is the absolute invariant that during processing of a batch of events in the event loop, every single non listener (or connection) data.ptr is NOT stale.
this is due to the fact that the connection objects on the heap are only ever removed in reap(), markdead never deletes and only sets alive = false.
so accessing the alive field of the connection object to stop further processing if not alive allows every method in processevents to directly take a raw pointer to the connection object as its parameter. this saves redundant lookups inside connectionsmap since by design, they are guaranteed to be present. so the invariant and the alive checks are doing a lot of heavy lifting.

markdead also follows above principle, every markdead call within processevents or even in reap() is BEFORE the actual toremove iteration removal.
read up on the doubly linked list implementation for inactive auth client cleanup, will be implementing it next...

## 26/08/26

Slow progress, was occupied with other work. Implemented the intrusive doubly linked list to evict authenticated (or related state) clients that have not sent a heartbeat/valid packet in some time.
the idea is simple: have two sentinel permanent nodes that are initially on startup linked to each other. whenever a client transitions from unauth-auth, then always attach before the tail. a generic 4 pointer operation pattern ensures that we do not need to check cases of the list being empty etc. (because we only ever deal with tail, for the empty case, tail.prev is head automatically...). if a client sends a packet, then simply take their node and move it before the tail since no node is newer than a client that just notified.
when marked dead, simply unlink the node and connect its next and prev nodes together.
this ensures that in reap(), we can just check the nodes linked to head while the list is not empty (head.next != &tail), if timed out, then unlink and check next, otherwise break. O(1).

another neat thing is the intrusive part. normally to reach the node itself, one would need a hashmap, but here the connection objects become the nodes themselves since they contain the next and prev connection pointer. so when dealing with a connection object, we automatically have its node.

with this implementation there was only one major dilemma. I learned about it and introduced it because it seemed like something worth learning and implementing. but considering the alternative was just to traverse all the connections, check how long they have not been heard from and evict/continue, that would be at most 10k operations per reap cycle. so considering an epoll_wait timeout of 1 second, 10k iterations per second at max capacity.
but then considering that unlink and shift to end for a client that notified requires 8 pointer operations per msg. then if clients average 8 msg/s, then it means 80k pointer operations per second at max capacity. I realise those operations are very fast, but so is the contiguous vector iteration over a small cap like 10k?
one small argument is the fact that maybe I would increase the max users cap in the future or reduce epoll_wait timeout, in which case the scan does much more work.
nonetheless, I am keeping the DLL implementation. the natural text step is a timing wheel, but I am skipping it.

with DLL implemented, I needed epoll_wait timeout to prevent reap() from only running when some event is set on some client connection.
but a fixed timeout was bad because it meant periodic sweeps even when there is noone in the unauth queue or the list.
so the better alternative is to actually have a dynamic timeout based on the least expiry time between an unauth client and an auth client.

reviewing the code also led to me finding quite a few ordering bugs (for example, I had shifted state change from NONE to UNAUTH only when a new client actually passes the ctl add, but I had placed the statechange (whose method switched to accepting raw connection pointers for DLL support) below the newconn std::move), which again points to the fact that I need to actually start testing the server with a small client helper.

also adopting a stricter policy to keep myself sane and not keep on switching how the code flows. basically methods operating on a connection object now have a contract on what they need to properly function, which is implemented via a SERVERASSERT, and its the caller's duty to ensure that it is not violated. this makes it much more cleaner than deciding where to put checks at (in the method or at the callsite). of course this is only in regards to what the architecture says must be true and not what a hostile client can determine.

the write path has been pretty straight forward till now. the same scenario of using a writepointer for how much has been written to it and a sentpointer for how much of it has actually been sent to the kernel send buffer.
but there was an important design flaw.
at every sendmessage() site, the program checks if the message to be sent can fit inside the buffer or not via MAX - writepointer, and if not, the client is just too slow and dropped.
flushwb() tries sending everything inside the buffer, and on partial sends, it increments the sendpointer and stops. this is bad design and the solution is something I already do in the readbuffer.
the problem is that sendpointer advancing in a flushcycle on partial write, leaves unreclaimed space at the front of the buffer. if a client wants to send the entire buffer worth content but is only able to send say around 97% of it. sendpointer increments leaving 97% space at front while writepointer stays at the end.
the next cycle, the client gets dropped due to insufficient space on message send even though space is there at the front.
the fix is to prevent 0 byte sends at the start and memmove on partial writes.

## 27-29/08/26

Before implementing sessions and the database (sqlite3), I have been delving into threads.
The thing is that hashing and even some batch writing, read operations can be too expensive to put inside the main epoll loop as it will introduce latency for all the clients in a batch.
this means that a pool of threads is needed to handle expensive work. an N thread pool for hashing and a single database thread for read and batch writes.
I realise that this adds complexity, but implementing it makes the other parts simple.

--
Starting to implement a threadpool and a thread safe queue structure, and I am already having to digest alot of c++ information.
the queue has to be generic in the sense that it can be initialized with a generic type like a JOB struct or a RESULT struct. this can be achieved by using a template class.
push and pop inside the class that deals with the queue<T> also has to take T as a parameter.

a simple push operation needs to take T as a parameter.
3 options --> T& job, T&& job, const T& job
to simplify things, all queue operations work on the contract that job cannot be an rvalue at the callsite. this is guarded by the compiler.
in my architecture, a job is something to simply be pushed and forgotten.
this means, that const T& job is the wrong choice because queue.push() cannot have std::move inside it as a const reference cannot be moved.
so after acquiring the mutex, this version would create a deep copy which is wasteful.

the T& version is good considering the contract of lvalues only, since it leads to only 1 move operation and 0 copies. but the problem with this as I learned is that, std::move on the reference inside push() essentially destroys the lvalue job in the caller. although this holds consistent with the fact that job is not required anymore regardless, a better alternative is if the callsite 'knows' that the lvalue will be gone.
this is possible with T&&. this version for lvalues is the same as T& but it requires the callsite to move the lvalue in the argument -> queue.push(std::move(job)); which signals the death of job.

but all of above is virtually insignificant for the JOB struct atleast I am thinking of having. heap allocated resources are absent from it, this means that the copy constructor and the move constructor are identical in practice, so converting the lvalue reference to an rvalue one using std::move inside push() so as to pick the push overload that moves instead of copying is not of much help.
it also means that nothing really gets destroyed at the callsite, that issue only arises for heap allocated fields, but right now there are none.
but I am keeping T&& just in case future edits start adding complex structures inside the job.

--

Getting into argon2 hashing. The idea is simple. client will send plain text username and password over the network. transport layer security is something I am not handling, so assuming security over the wire -> a login attempt is a packet send over the socket that the server always considers unauth at first.
the login packet contains the credentials. the server loop handles login by creating a job and giving to the DB thread. now there are 2 things that can be done, either the DB thread does the fetching of the argon2 string stored on the disk and create the hashjob therein, or it returns a result, and based on the result the main loop will create a hashjob. I am not sure about the complications involved with the first method but it seems 'better' even though the second one feels 'safer and right'.

the hash pool basically pops a hash job, use the password plain text and the argon id to verify if based on the same salt parameters, the password genuinely converts to the same string and then stores the result. the main loop finally reads the result and if the connection is still in connectionsmap (not timed out), it promotes it.

This comes with the understanding that from now on there will be two types of handlers, those that can respond immediately to the client and those that only cover the portion which handles off a job to the threads, and then continues to the next client. the other part of the handler is in the section that processes the event set by the threads on result.

--

Ran some testing. since the project is on my laptop, I will use the benchmarks produced on it, and thus all values of the hashing parameters are based off it.
considering a HASH that takes T time, and there being N threads in the pool, it takes T seconds to complete N hashses.
T->N
T/N seconds to complete 1 hash.
if the number of jobs in the hashqueue is M, then total time taken is M x (T/N).

a connection that joins has a 5 second deadline to promote. this means that the hashqueue needs to have a cap on the maximum number of jobs at a time, otherwise jobs for connections long back in the queue are useless since the connection expires before they complete.
with a 5 second deadline, M x (T/N)  < 5
so , M < 5 x (N/T)

T was interesting. to compute it on my laptop, I ran 1200 hashes on a fixed password on 4 threads 5 times, and:

```
Total time taken: 108017ms.
Average time taken: 90.0143ms.
Median time: 88.0645ms.
P99 time: 110.687ms.
Longest hash took: 121.618ms.
```

for T, P99 is the most sensible since it talks about how much time 99 percent of hashes took. at 4 threads, this means M ~< 181.
with ~180 hash jobs, the queue is barely fully processed for the last waiting client connection job.
so the actual cap must be comfortably smaller. I am settling for 100.
at 100, the hashes are completed in (100 x (T/N) time) which is around 2.76 seconds, well under 5 second deadline for the last job.

## 4-5/09/26

Implemented the threadpool as a test again, which finalized the queue structure. earlier, the queue had its own mutex while the threadpool had one as well along with the condition variable.
I replaced this with a better pattern that made the threadpool structure much simpler. 
the queue owns the condition variable and a mutex. a push notifies a thread to wakeup.
two pop methods inside the single template queue struct. popblock() is for the hashpool where it blocks on a condition variable if the queue is empty or if the kill signal is not true.

the other one is for the receive queue, popping from which is a normal try and pop, since the main loop periodically pops from it. 
After finally implementing this, I also got to change the structure of parse because till now it did not return relevant information that the handlers needed to read their message (parse was only returning the message type). now it returns a struct. 
A basic signup handler has been implemented that checks for non terminated pass,user,email buffers and will then pass on to the DB thread which I am moving on building towards next.

Interestingly, I found something about templates. sendmessage is a template method. and its implementation was in server.cpp. since handlers.cpp is another file that can call sendmessage(), it gave me a linker error. 
turns out I had to move the definition of sendmessage inside server.h itself below the class for anyone including server.h (handlers.cpp) to be able to find the code.
via server.h, handlers.cpp's function call was able to see the template definition so the compiler did not provide an error, but the linker could not find the implementation for template instantiation since it was sitting in server.cpp.

moving towards implementing sql and the DB thread, and shortly after sessions as well finally.

## 11/09/26

Delved into basic sqlite3 and its configurations. right now the workflow itself is a bit unorganized in terms of what to implement next. The plan is to add a database struct that will hold the DB thread as well as the methods to execute the prepared statements.

to determine how sqlite behaves, we need to set up its pragmas during init. These are set normally like executing a query. one of them is journal_mode = WAL.
so I read up on it and WAL (write ahead logging) is quite interesting. 
my dumbed down understanding of this is that writing to the disk server.db file on every write job by the db thread is expensive and it also blocks any other reading thread while the main file is being written on. 
WAL basically takes whatever is to be written and appends it to a separate file called server.db-wal and not the main file. when the number of entries inside this new file get large enough, checkpointing occurs where the contents are finally flushed to the main database file. 

WAL allows for writers to not block readers. The exact jargon is not clear to me, but the idea is that the WAL file contains the most updated truth about a row in the main file. the gap between the WAL file and the main file is filed by server.db-shm which contains indexes that we can use to reach the updated row in the WAL file if it exists (because if so, this is the content that we want). if it does not exist, we read from the server.db file.
if at t=0, a reader wants to read row 6 which gets mapped to unit 80 in the WAL file, they are set to only read from unit 80 even though while doing so another writer writes the most updated info about row 6 in unit > 80. these are only ever appended and the shm index ensures that readers post updation are guided to the units at the end. this way readers can read and writers can read. while a writer is writing, the shm updation does not happen so someone cannot read midwrite. 

...and after all of what I just wrote, in my case, there is only a single thread that does both reading and writing and NEVER together. but WAL also buys fewer fsync calls. fsync forces the operating system to basically take the server.db file in the page cache and then flush it to the one on disk so that the changes are permanent. so this is slow.
in WAL, fsync is only called during checkpointing. all the frames are written to the server.db cache and then finally a single LONG fsync call pushes them to the disk. this is the more significant reason for using WAL.

--

Realised I cannot use the threadsafequeue structure for the dbthread read & write queues considering the fact that each one contains its own mutex and condition variable. this means that the db thread sleeps on two separate conditions. thus a read can block writes that are present in the writequeue. It's better to just create a separate struct that will contain both queues and a single condition variable. 

## 12/09/26

The next problem after having a separate queue for the db thread, is that this queue can only hold a single type of item, which will be the readentry struct for the readqueue and the writeentry struct for the write queue. 
but considering the readentry, it needs to hold the connid, the readjobtype enum and then the parameters that will be needed to replace the '?' placeholders in the job specific prepared statement. 
so a single struct now needs to hold variable combinations of parameters.
Thought on this a bit and reached the conclusion that this can be done if I follow what I was doing on the wire. basically, I can create a byte buffer that has the size equal to the maximum parameter combination size (currently, no statement uses more than one parameter buf if in the future it does). 
then the dbthread upon receiving the job can simply switch on the jobtype enum and then get to know how to interpret the byte buffer and mempcy bytes into known parameter structs (like usercacheparams or verificationparams).
but of course this would need me to maintain the max size variable, which I can eliminate by calculating max of the param structs at compile time. for safety, I can also check if a newly added params combination struct in its handler has size greater than max size if it was not used in the calculation by mistake.

--

Above works, but there's something cool I found on the internet, a union. Had never used it but it is similar to above but avoids memcpy. turns out I can store the params struct directly inside a union field in the readjob. the union will automatically have space for the maximum sized params struct so no calculation needed. 
the main thread when creating the entry will have to set the jobtype and then the contents of the union as well.
and then inside the db thread switch, it can directly access the union struct based on the enum without memcpy.
the only thing is that the entry creater (main thread) and the db thread handler have to constantly agree on what to put and read based on the enum. NOTHING prevents me from accessing the wrong params struct from a union that is not filled with it.

--

which brings me to std::variant. always heard of it, never used it. turns out it's best suited for my use case. It does all of the things that union does above, but it allows me to use std::get_if<T>(variant) (this version does not throw an exception) to simply check if the type I am asking for is the one the job's variant field holds.
I was avoiding std::visit because looking into it brought forward a template pattern that seemed very complex. even though I worked it out, I think I am going to implement my own functor with all the operator overloads per param struct to pass to visit. 
std::visit ensures that no type inside the variant is left unhandled in the functor.

# 24/09/26
Been working on this for a while now, but the cross communication between main thread, db thread, and the hashpool is genuinely hard and has made me pause for a while and take it slow.
For the login path, I had decided I would chain operations in the sense that the main thread would create a db lookup for the username, and then instead of the main thread awaiting the result and then creating the hashjob, the db thread on successfull lookup would create the hashjob and pass the retrieved account id to the hashjob entry. 
Once the hash is complete, the result of the hash will contain the account id that the main thread can use to create a session if the connection object still exists.

so this meant that the database was not interacting with the hashpool. for sign-up though, the problem is different. it makes intuitive sense to hash first from the main thread because the db write to enter a new user requires the hash string. 
but in trying to chain the hash job so that it could create the db write job directly, I simply stopped. Because doing that meant that the hashpool now had to have a reference to the database to submit a job. which also meant the hashjob would now have to carry information that would be required by the write statement.

I realised that atleast for the sign-up path, this was actually making things complex where otherwise the hashpool just took what it needed for the hash. It makes no sense for it to carry email and username fields.

sign-up will hash first. and then use the result to make the main thread issue a write job. the critical details need to survive while the hashjob is being executed. this can be done by making the connection object hold buffers to what is user provided on signup (username, password, email). the connection object is getting quite fat, will probably make it much smaller for unauth objects and only give the full thing on authorization...

Another thing was about the early failure case of login where the username is not found. hashing on success and not on failure creates a time mismatch. so the standard fix is to hash regardless but in the failure case, use a fixed hash that is bound to fail.
But I am not doing that. usernames are public by default. so signup path can have a lookup before the write job to check if the username already exists (the latency is nothing compared to the hashing that would take place for a new user) and returns a message if it does. so an attacker DOES know which usernames are valid via signup. but I am relying on my hash queue caps and the rate limits. So, since enumeration is not a problem, hashing against a decoy on login lookup failure just costs me resources via the hash, it will not be done and just a generic incorrect credentials message will be returned quick.

# 25/09/26

DB result queue now has a separate mutex.
Processing all read and write jobs while holding the wait/read queue mutex was wrong. so this was fixed by acquiring the mutex, swapping all the read and write jobs into a local read and write queue declared at the top of work() before the loop. the lock is then released and these local jobs are processed by popping until empty.
This also naturally led to having a local result vector. the idea is that this allows me to put the results returned by std::visit in the read and write processing loops into the local vector.
then iterate that to push the results into the main result queue.
I wrote it wrong the first way around by processing all read jobs first and putting the results in the local vector. then processing the write jobs and putting it again inside there and then finally flushing. but that would add alot of latency for readers that expect fast response.
so we process reads, flush the local results to main result queue. then process writes, and do the same.
Because resultqueue is an std::queue, thus the local result vector method does add N extra copies (pushing into the result vector, and then pushing into the main queue). it also adds heap allocation (BUT, since job counts are implicitly bounded by connection counts and rate limits, reserve hits a ceiling and essentially becomes free in further iterations). The theoretical win is that it avoids opening the resultqueue mutex N times for every result push. but contention is genuinely small because only the db thread and the main thread touch that mutex. I am still keeping the vector design, because the copies are virtually free given the size of the dbresult structs and that they are entirely trivially copyable. 
It actually does help in the cases where contention happens.