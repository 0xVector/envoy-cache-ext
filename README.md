# Envoy ring cache filter

## Usage

### Building

To build the Envoy static binary:

1. `git submodule update --init`
2. `bazel build //:envoy`

### Running

To run the Envoy binary with the ring cache filter using the [`cache_filter.yaml`](cache_filter.yaml) configuration file:  
`./bazel-bin/envoy -c cache_filter.yaml -l debug`

### Tests

To run the unit tests for the ring cache filter:  
`bazel test //ring-cache/test:unit_test`

To run the integration tests:  
`bazel test //ring-cache/test:integration_test`

### How it works

The [Envoy repository](https://github.com/envoyproxy/envoy/) is provided as a submodule.
It is pinned to the commit `8283565cffc7b713ef1b3a8b79c285c269e15db3`,
which is the commit used by [envoy-filter-example](https://github.com/envoyproxy/envoy-filter-example) - later versions
of envoy change the docker scripts and don't work with this setup out of the box.  
The [`WORKSPACE`](WORKSPACE) file maps the `@envoy` repository to this local path.

The [`BUILD`](BUILD) file introduces a new Envoy static binary target, `envoy`,
that links together the new filter and `@envoy//source/exe:envoy_main_entry_lib`. The
`ring_cache` filter registers itself during the static initialization phase of the
Envoy binary as a new filter.  
The [`ring-cache/BUILD`](ring-cache/BUILD) describes the library target which provides the filter.

### Try it

To try out the cache functionality in a live scenario, you can use the provided [`slow_upstream.py`](slow_upstream.py)
script that simulates an upstream taking longer to send all responses. If you provide it a query param `?chunks=5`,
you can control how many chunks of body it slowly sends out (to e.g. test coalescing behaviour).

```shell
# Run the upstream
python3 slow_upstream.py

# In a different terminal - run Envoy
`./bazel-bin/envoy -c cache_filter.yaml -l debug`

# In a third terminal - client 1
curl -N http://127.0.0.1:10000/test?chunks=20  # Send a request for 20 chunks

# In a fourth terminal - client 2
sleep 2  # Wait while the client 1 receives some chunks of the request body
curl -N http://127.0.0.1:10000/test?chunks=20  # Send a request for the same path
# Now should immediately receive all the chunks that client 1 received

sleep 10  # Wait till the whole body is received and cached
curl -N http://127.0.0.1:10000/test?chunks=20  # Send a request for the same path again
# Should get the whole body immediately because it was cached previously
```

To try out the eviction behaviour live, try setting the `capacity` cache config param in [`cache_filter.yaml`](cache_filter.yaml) config file
to a low value (in bytes) and request multiple different paths to fill the cache. The oldest (most usually) entry should
then get evicted and will again have to be fetched from the upstream (which will try to re-cache it).

### Config

The cache uses a standard protobuf config with 2 configurable parameters:
- `capacity` - cumulative limit on cached response size in bytes
- `slot_count` - limit on the number of distinct entries in the cache

These can be set in the YAML config file.

## Goals and Non-goals

This project has several goals:
- to provide a thread safe global cache of responses
  - that has bounded cache memory usage
  - and does request coalescing
  - and is capable of evicting cached entries
- to keep the implementation simple
- to provide a proof of the concept that implement the most important and interesting parts
- to prefer performant solutions where other concerns don't take priority

It also has these non-goals:
- to provide a production-grade cache
- to make all possible performance optimizations (at the cost of increased complexity)
- to respect all details of HTTP caching
- to be security-hardened

These were selected based on the task specification and in an effort to limit the scope of the task.

## Architecture overview

The whole source is in the [`ring-cache`](ring-cache) directory.

### Existing extension

I decided to write the whole cache by myself. An alternative to this would be to use the work-in-progress and not production
ready HTTP cache extension.

As of now, it is composed of a cache class implementing the HTTP caching standards, control headers
etc. and an interface to add cache storage plugins. There are storage plugins right now, a non evicting in-memory storage
and a filesystem storage. I did not use any of this code, but I used it as a reference and inspiration for my own implementation.

**Rationale:** I heavily considered building on top of the existing extension. An advantage would be that whilst not production
ready yet, it already respects many of the nuanced parts of HTTP caching that I am not implementing myself.  
I decided against it due to concerns about existing code complexity. I think writing my own simplified cache was a quicker
way to get some results, compared to studying the existing code in detail to figure out how to extend it.  
Another problem is that the extension doesn't solve my problem exactly - it doesn't seem to support coalescing, adding
which might entail changes to the cache interface, eliminating the value of having a ready implementation. The simple in-memory
storage can't evict entries, so it's not suitable as well and would have to be modified or even redesigned.  
Overall, I found my own cache preferable for this task, but would heavily consider the extension for production code.

### Filter

The filter is implemented in the [`filter.cc`](ring-cache/filter.cc) as a class implementing the `Http::StreamFilter`
interface, thus it is both an encoder and a decoder.

The filter instances and request streams are grouped into 3 distinct categories - roles:
- Hit (when a response is fully cached -> can be handed out immediately)
- Follower (when a response is not cached, but is inflight -> can be coalesced)
- Leader (when a response is not cached and not inflight -> an upstream request has to be made)

**Rationale:** I also considered a simplified flow where there would only be a simpler, unified 2 role API: consumer filter
either gets filled by the cache (transparent to it whatever it is a hit or a follower), or becomes a leader. I still think
this is a valid option, but in the end, I decided to use the 3 role API to avoid having to go through the dispatcher event loop on
cache hits too - that path is necessarily a bit slower and I consider hit latency to be important.

The filter is meant to be lean and offload most functionality to the `RingBufferCache` class. It tracks the basic
state of its stream and uses the `RingBufferCache::lookup` function to determine the caching status for a request.
This happens when decoding downstream request headers, ignoring the request body, trailers and metadata.  
On upstream response encoding, it is responsible for passing the headers / body data back to the cache so it can be
cached and handed out to followers that could be coalesced. The filter (in a leader role) is then left to use the
upstream response for itself. Other filter roles are not meant to receive a response directly from the upstream.

**Rationale:** I wanted most of the caching logic to be contained in the cache class and the filter to not be too
cluttered for future extensibility and encapsulation (cache internals can be improved without having to rewrite the filter).
The filter is coupled to the cache to a degree (e.g. non completely trivial API calling contract, see the cache API part for more details),
but this isn't that big of an issue as long as the filter is still small - even a rewrite of the filter, in case of some breaking cache API changes,
wouldn't be too big of a cost.  
I chose to make the filter aware of its category (role) because we need some behaviour changes based on the role.
The filter needs to at least return the correct `Filter..Status` value (which are different) and post the upstream
response only in the Leader case.

#### Key building

The key is built in the filter itself. It is a simple concatenation of the host and path with a delimiter.  
The delimiter can be reconsidered in the future, as invalid delimiter choice opens up a range of attack vectors by altering
the cache key by changing one of these components to resemble a part of the other.

### Cache

The cache itself is implemented in the `RingBufferCache` class in [cache.cc](ring-cache/cache.cc). It provides a simple
public API that the filter can use to utilize the caching functionality.

The cache uses bounded memory to store responses which can be set by the `capacity` parameter.
It doesn't take into account the inflight coalesced responses at this point, which is a limitation that could be
removed in the future, at the cost of some added complexity.

#### Singleton

The cache class is registered into the Envoy singleton system to easily provide a single, shared global instance as per
task statement.  

I also considered an alternative design: having a thread-local cache instead of a global one. As per the task statement,
I avoided this design. Its advantage would be a simpler cache without locking and synchronization overhead, at the cost
of significantly increased memory usage due to redundantly stored responses. This might be a good tradeoff in some cases,
but as we can't influence what thread receives what stream, it's not a good fit here.

#### Caching format

The cache takes a simplified approach to caching the responses: it ignores trailers, 1xx headers and metadata.
It also ignores the response header semantics and doesn't tweak them in any way, just caching them untouched.  
It also doesn't take into account HTTP methods - in a production environment, generally only `GET` and `HEAD` methods
make sense to cache.

**Rationale:** this simplifies the problem of caching and lets me deal with the interesting parts of the task.
Trailers are not very common in HTTP requests and are very similar to headers from the technical standpoint. Their
support wouldn't add much of value.  
Similarly, while differentiating between HTTP methods is trivial, adding a simple exclusion list is not a particularly
interesting feature.

#### Coalescing

Apart from serving cache hits from stored entries, the cache can coalesce requests with the same key at a similar time
to a single upstream request.

This is achieved by receiving `StreamDecoderFilterCallbacks` from the filter instance and `post()`-ing to its dispatcher
with an appropriate `encodeX()` call. The coalesced followers are backfilled immediately (almost, as the encode calls
must run on the follower thread, so they must pass through their dispatcher event loop).  
When new data arrives from the Leader, it is also published to the followers as soon as possible. Followers thus receive
the data almost at the same time as the leader. New followers can join and get coalesced as long as the response is incomplete.

The coalesced request also be cached permanently after it's finalized.

#### Data structures

The cache uses these internal data structures:
- fixed sized (`slot_count` config param) ring buffer of unique pointers to cache entries
- hash map of keys -> cache entries for fast lookup
- hash map of keys -> inflight entries

All of these structures are guarded by a single internal mutex.

**Rationale:** My interpretation of the ring buffer cache from the task specification is that of a fixed size buffer of pointers to entries.
I picked this implementation because of several reasons:
- it allows for simple eviction / free slot allocation via a single straightforward scan
- cache hit serving is fast, as only a no-copy `BufferFragment` is constructed from the existing entry (and the header is copied, which is presumed to be small enough for it to be insignificant)
- any data structures can be used for the actual data inside without having to serialize/deserialize or otherwise transform
  the data - in particular the `ResponseHeaderMap`

There are some drawbacks to it too, though:
- to satisfy the fixed memory requirement, manual memory accounting has to be done
- there is one extra level of indirection
- increased allocator pressure from dynamically allocating the entries

I also considered these alternative possibilities:

1. Fixed size ring buffer of raw headers + data. This sounds like a more straightforward interpretation, but I
   decided against it due to these concerns:
    - Envoy's `ResponseHeaderMap` would have to be dumped into the buffer, then deserialized on cache retrieval. This
greatly increases the complexity compared to just copying the whole header.
    - entry lifetime management would be harder to implement correctly, as usage of entries would have to be tracked
      separately from the raw data store.

   Advantages of the raw ring buffer would be:
    - less allocator pressure
    - better cache locality - raw header and body can be grouped together, less indirection

2. Fixed size ring buffer of just raw data + ring buffer of pointers to `ResponseHeaderMap`s.  
I decided against this as it combines the manual memory accounting of my approach with the complexity of
the raw header + data buffer, in other words, the worst of both worlds.

#### Thread safety

The cache class public API is thread safe. The thread-safety is built around a single mutex and some atomics.  
`encodeX()` calls are never called across threads, which is strictly forbidden by the Envoy thread model.

**Rationale:** The decision to use just a single mutex is due to several reasons:

1. Apart from data structure operations, the mutex also guards internal invariants. Splitting the mutex to several
   finer-grained would break or severely complicate this. An example is that waiting followers are always fully backfilled
   when the mutex is not held - this greatly simplifies the backfilling process, and allowing it to be guarded by a separate
   mutex to e.g. the one guarding `lookup()` would easily violate it. Many of the invariants are interlinked like this and
   hard to separate to multiple mutexes.
2. Complexity. Multiple mutexes increase design complexity and care must be taken to avoid deadlocks.

Considering these, I found it reasonable to use just a single mutex at this stage while trying to limit critical section size.

Simplicity of implementation is at time preferred to strict multi-threading performance. For example, some critical
sections could be made smaller at the cost of redesigning some flows or even changing some invariants. I tried to always
pick the most sensible option, not necessarily the one with the best performance.

#### Memory usage

The implementation tries to minimize the memory usage, most importantly by limiting the number of copies of processed
data. More specifically, it uses Envoy `Buffer`s wherever possible, which can be backed by shared `BufferFragment`s.

The most significant memory use is from storing copies of responses (specifically the bodies) for caching. Copies are
created in these places:
- when Leader publishes headers/data, it is copied to the inflight store.  
For headers, this is because the `encodeHeaders()`
  takes ownership of the `HeaderMap` and thus needs its own copy.  
Data could be moved from if `StopIteration` was returned, and then
  the Leader could be treated the same as Followers, eliminating the need for this copy. This could be an addition in the future.
- when publishing headers, a temporary copy of the chunk is created (in addition to the inflight copy mentioned above)
  for each waiting Follower.  
This is because the filter expects its private copy of the headers that it can take ownership of (in case subsequent
  filters wanted to modify it).
- when publishing data, only a single extra temporary copy is created.
  This copy is efficiently shared between waiting Followers via a `BufferFragment`.  
This additional copy was chosen to
  simplify the implementation - as the Followers are backfilled without the mutex held (and asynchronously via `post()`
  call to their dispatcher), the inflight entry can be modified or even dropped / moved to permanent cache at any time.  
  The inflight entry is stored in a std::string, which makes it easy to manipulate, but can be reallocated at any push of new
  data, which intensifies this. Overall, I consider this a sensible tradeoff to shorten the critical section and decrease
  complexity.

Notably, data is **not** copied in these significant instances:
- when an entry is finalized and moved to permanent cache, the headers and data are moved from the inflight store with
  no unnecessary copies.
- when serving a cached hit, the data is not copied from the cache store, instead a `BufferFragment` is used to just
  reference it without copying.

The cache has bounded memory for cached responses, but doesn't limit the size of inflight responses for now.  
The memory is accounted manually on entry creation/eviction.

The size of an entry is calculated as follows: `key_size + body_size + number_of_headers`.  
The headers could be accounted by the real size, but this doesn't seem to be straightforwardly provided by the `HeaderMap`
objects.

#### Resource lifetime management

The cache manages the lifetime of its entries by storing a simple reference count. Entries still in use
can never be evicted, which guarantees correctness.

**Rationale:** I considered alternative solutions, such as the cache storing shared pointers to whole entries. I decided
against that, as while that would be a simple and correct implementation, it would cause the memory limit to become a soft
limit - referenced entries could linger in memory for unpredictable time periods after eviction from cache.  
If we accept a softer memory bound, this might still be a good alternative as the cache user can be considered responsible for
the memory consumed after eviction.  
On the other hand, a disadvantage of my solution is that any live reference to the cached entry can block the cache
indefinitely, which can be unintuitive to the user.

Internally, when handing out chunks to coalesced Followers, shared pointers are used in conjunction with `BufferFragment`s
and custom deleters.

**Rationale:** this ensures that the chunks stay alive just long enough in the unpredictable environment of dispatcher
`post()` calls, where the order of execution can't be clear beforehand.

#### Evictions

The cache uses a straightforward scan algorithm to evict entries. The eviction start with considering the next slot,
which usually means the oldest entry. Entries in use are just skipped. It is behaving as a FIFO container with some exceptions - a younger
unused entry can get evicted if the older ones are still in use.

**Rationale:** this was designed primarily with simplicity in mind. More performant eviction algorithms (for example LRU)
would need to store more information to work. This works with just a single head for both writing and evicting and needs
no extra information.

### Cache API overview

`lookup(key, callbacks)` - is called as the first stage of every new request stream. It atomically determines the outcome
for this key and returns a result with the appropriate category (`Hit/Follower/Leader`, see above). `Hit` results also
contain the cached data and can immediately be used to fulfill the request, whereas `Follower/Leader` results are more
like a marker instructing the filter to wait for further processing.

`publishHeaders(key, headers, end_stream)` - function for publishing the response headers by a `Leader`. This
triggers the backfilling of coalesced `Followers` and can also finalize the response if it's the end of the stream,
moving it to the permanent cache.

`publishData(key, data, end_stream)` - similar to the function above but for publishing response data. Has to only be called
after a call to `publishHeaders()` for the key that hasn't been ended yet.

`removeWaiter(key, waiter)` - a function to signal that the filter is ending its lifetime prematurely and will not
process any new data. 
This must be called in the case the filter is being torn down before the whole request is finished (i.e. in `onDestroy()`)
to avoid the cache calling `encodeData()` on an invalidated stream callback instance (for example
when the client closes the connection).

The public API has to be called exactly in the manner prescribed per stream category:
- Hit: lookup()
- Follower: lookup() -> (if ending prematurely) removeWaiter()
- Leader: lookup() -> publishHeaders() -> (if not header only) publishData() -> ... (till stream not ended) -> publishData() (last call has to end the stream)

## Performance overview

The purpose of a coalescing cache like this is to improve the performance of a system by reducing the load on the upstream.
This is done in two ways:
1. caching the responses to not need to contact the upstream when the same request was already
fulfilled recently
2. coalescing identical ongoing requests to mitigate the thundering herd problem (a burst of requests exhausting upstream resources)
and speed up responses in some cases with a slow upstream.

My implementation achieves both of these goals. As with any performance optimization, there are tradeoffs. Some of them
are inherent to a caching system like this, while some others are a result of my implementation and could be potentially
alleviated.

### Latency vs Throughput

The effects of a cache on latency and throughput are highly dependent on the parameters of the upstream.
With a slow upstream, the cache can greatly improve latency for cache hits and even the first parts of a coalesced request.
Not only does the client not need to wait for the slow upstream to produce the responses, the cache can also be closer in
the network and reduce the latency in this way as well.
The throughput will also be improved, as the upstream will have fewer requests to deal with thanks to the cache shielding it.

With a very fast upstream, these tradeoffs change: latency won't be decreased much, or may even be increased - the cache
has some internal overhead and the upstream can be potentially more performant. The coalescing can still be a strong
improvement though, as the throughput gain can scale nicely with the amount of burst requests for the same key.

### Lock contention

As the cache uses only one mutex internally, this can become a source of significant overhead. The tradeoff and
reasoning for this is explained in the **Thread safety** part of the **Architecture Overview**.

Lock contention becomes significant under heavy request load. Different request streams can run on different worker threads
in Envoy in parallel, but they will fight over the single mutex on all cache functions that hold it. This means that e.g.
completely different queries with a different key can't be served concurrently.

While the critical sections could be shortened in a few cases by a better or more complex design, the single mutex will
always be a hard limitation. The most promising solution I see is sharding the cache: when using a good hash function
that behaves random-like (enough), the hash space split evenly will result in an approximately evenly split key space.
In other words, keys will be mapped to their cache shards uniformly, resulting in a uniform load.  
The cache shard can be just smaller instances of the current cache class, only serving the part of the key space that
hashed to a specific hash range. This could nicely lower the locking overhead, as multiple different keys can be served
at the same time while still providing global caching.

### Memory overhead

The cache has some overhead when storing the response entries. This overhead is minimal - it only stores the key and
a reference count in addition to the useful response data. The key could be eliminated by a more sophisticated
solution which uses some other way to find the map entry when it is being evicted.

Inflight entries have a bit larger overhead, as they also store information about coalesced followers.

### Watermarking

Watermark buffers are an Envoy feature that aims to provide a robust and general solution to flow control.

Buffers have two configured limits - a high and low watermark. These limits provide a soft cap on the buffer capacity - when a buffer
exceeds the high watermark, it informs its data sources of this by calling a registered callback. The data sources are
then expected to stop sending more data to the buffer, allowing it to drain. When the buffer is drained enough, it hits
the low watermark and again informs the sources by a callback. The sources can then resume sending more data.

My implementation doesn't call any watermarking callbacks as it did not fit into its scope. It could be added as a future
improvement.

#### Coalescing

The main flow control concern this cache creates is during coalescing: a single upstream chunk (even though not copied
directly in the internal cache buffers) is given to potentially many followers and into their downstream network buffers.
The risk is that in case of even a follower hitting the high watermark, the whole upstream stream has to be paused till
the slowest follower drains, otherwise the follower buffer can run out.

To support watermarking in coalesced requests and fix this issue, the following things would have to be implemented:
- when a Follower is attaching, a cache should attach a watermark callback to handle the high/low signals
- when any Follower signals the high watermark, the Leader should propagate this to the upstream to pause it
- when all Followers hit the low watermark, the Leader can resume the upstream again

#### Cache hits

Another watermarking related issue happens when serving a cache hit. In this case, the cache acts as an upstream itself,
and thus should respect watermark signals from its consumer - when the stream hits the high mark, it should pause
and wait for the consumer to drain before sending more cached hits.

The implementation doesn't chunk the cached responses at all, which can immediately overshoot the high watermark with
bigger bodies. This should be addressed with the watermarking and chunks should be paused when a high signal is received.

#### Flow control in different HTTP versions

While the implementation of watermarking when coalescing can be the same, there are some significant differences to
flow control across HTTP protocol versions:

**HTTP/1.1** has no built-in flow control primitives and relies on TCP flow control. Individual requests usually have their own
TCP connections, which means that a slow client doesn't affect others. Coalescing changes this though, as a single slow Follower
can block all others for they same key. The backpressure comes from the
downstream connection socket buffer.

**HTTP/2** connections multiplex multiple requests streams into a single connection. Due to this, it provides a built-in,
stream level flow control via a flow control window. In a nutshell, the size of the window is exchanged between the parties
and sender limits its output when reaching the end of the window. The backpressure can also come from this window.

**HTTP/3** introduces the QUIC protocol as a new transport layer protocol. It improves the multiplexing capabilities over
HTTP/2 by making multiplexed streams over a single connection independent and thus eliminating the HoL blocking inside a connection
that HTTP/2 displayed - when e.g a packet loss occurred in a single stream, all streams in the multiplexed connection were
blocked till retransmission. This also means that when a single slow follower pauses the whole coalesced upstream stream
for all followers, other streams multiplexed in their connections are not affected.

## Security considerations

The cache is not particularly security hardened, as I consider it more of a proof of concept than a production ready cache.
In this form, it is vulnerable to many types of attacks, most significantly
to DoS attacks, private data leaks and cache poisoning.

The cache does not limit the size of a single entry beyond the basic
total capacity, which could be exploited to evict many useful entries in favour of a single big entry.

Inflight capacity not being limited is also an attack vector: responses with many chunks can stay inflight
for a long time and exert memory pressure.  
Also, even though too big inflight responses (over the cache capacity) will not get permanently cached after finalization,
there is no limit on their size while still inflight, which means they could grow indefinitely and drain all resources.

The cache caches all responses, which can include user specific responses (that can be dependent on e.g. a secret in
cookies). This means cache users can steal cached private data of other users, which is a big security hole and would
need to be addressed in production.

Cache keys should get normalized before caching - for example, some parts of the paths are not case-sensitive and could
get cached redundantly. This can be misused by attackers to strain resources.

## Extensibility and maintenance

The code is split into the filter and cache parts. The codebase is not too large now, but for better maintainability,
the more complex cache class could be further decomposed into independent parts - for example, the existing cache extension
has a completely separate cache storage from the caching control, which are both merged in my class.

Envoy heavily uses the virtual interface - concrete implementation pattern, which could be useful in this case too, mainly
for easier testing and extensibility (parts of the implementation could be swapped out without breaking the API).

I tried to make the implementation relatively decomposed, used comments in more complicated sections and added assertions.
There is a room to improvement, but the code should be readable and extendable by other developers.

## Tests

I wrote several unit and integration tests for the cache. The tests are far from comprehensive - my main goal was to demonstrate
key behaviour is working properly.

### Unit tests

The unit tests focus on the cache class. They test some basic edge cases, test some invariants and demonstrate behaviour
that the cache provides - caching and coalescing.

### Integration tests

The integration tests check the end to end behaviour of the cache and try to simulate a few realistic scenarios.  
More tests, especially focusing on higher contention use could be added.

### Fuzzing tests

There are no fuzzing tests implemented at the moment, they could be a good improvement for the future though - tests
with real-looking patterns of requests could be helpful to catch more subtle bugs and test the cache in various load conditions.

## Limitations and possible improvements

### Main limitations

- the cache has bounded memory only for the cached entries, not for the inflight entries which are not limited in any way.
This has bad implications for security, but the fix is relatively simple and can be implemented as a future upgrade.
- the cache doesn't support trailers, metadata, 1xx headers nor cache control headers. These were all deemed as not absolutely
necessary to provide some results but could be added in the future, some easily, some a bit harder.
- watermarking is not implemented

### Current ideas and TODOs

These ideas were not realized in code yet, but I would work on them, had I more resources for the project.

1. a copy could be eliminated when the Leader publishes a chunk of data. The Leader could, instead of copying out
the data for the cache, move out of the Buffer provided to the filter and reuse its resources. It would then have to
`StopIteration` and get backfilled the moved data the same way as Followers do, eliminating a copy of data.
1. track and account for the inflight response sizes - this would be very similar to the existing cache entry size
accounting, just for inflight entries. A simple addition that could extend the cache bounded memory guarantee to inflight
entries too.

Some more specific ideas are documented as TODOs directly in the source. Some of these are possible refactors
which would simplify the implementation.

### Nice to have in a production version

The following ideas are a bit more complex, but would be very beneficial for a production release.

1. reserve space ahead of time based on the Content-length field (if present), merging several smaller allocations to
a single, bigger one.
1. configurable and more complex cache keys. The HTTP method should be included, among other things, as the same path with different
method can have varied semantics.
1. finer grained locking. The current single lock is simple, but doesn't provide the best possible performance. I'd
consider going in the direction of sharding - keeping the current internals, but splitting the hash space evenly
into multiple independent smaller caches that won't block each other.
1. stats - Envoy provides a way to report various runtime statistics. Reporting e.g. the current cache load could be
useful for testing, debugging or performance evaluation and general observability.
1. support for trailers, 1xx headers, metadata, response codes and cache control headers - these are all a must in a production
level cache, but do not fit into the scope of this task. Caching should probably be restricted to only responses that
are not user specific and can be cached (maybe even just GET requests).
1. much more testing - the provided unit and integration tests are far from a production-grade coverage of the cache,
but adding many more tests is again beyond the scope of this task. Fuzzing tests could also be highly beneficial.
1. security hardening - the security section outlines the main attack vectors, these all (and more) must be addressed
before any production deployment.
1. watermarking - see the section in **Performance overview**.
1. more configuration options - key, coalescing cap, individual response size cap, eviction parameters, cacheable responses... 
1. more robust public API - additional checks when called in a contract breaking manner, etc.
1. better eviction algorithm - e.g. something like LRU or similar.

## Development process

How I developed the code for this task.

### Roadblocks

I've hit several roadblocks during the development.

1. Building Envoy and Bazel: when I started, I had zero prior experience with Bazel and projects of the scale of Envoy
were a new experience for me. I had a lot of problems to get the Envoy repository to compile and also had trouble figuring
out where to potentially put my own code. I had problems with getting the codebase to even compile without hitting
OOM on my workstation.  
**How I solved it:** I found the [envoy-filter-example](https://github.com/envoyproxy/envoy-filter-example) repository
and forked it, then picked apart its structure. I also read up a bit on Bazel and figured out how to make a BUILD file
and throttle the memory usage of the compilation.
1. Orienting myself in the Envoy codebase - this was a big problem from the start, as I've encountered Envoy for the first
time.  
**How I solved it:** I read whatever sources I found on Envoy, then started reading relevant excerpts from the codebase.
Gradually, I figured out the structure of the codebase and understood key Envoy concepts.
1. Figuring out how to interpret parts of the task - I especially had trouble with the ring buffer storage structure,
and multi-threading approach.  
**How I solved it:** I asked a set of questions, which cleared it up and nudged me towards making the key design choices.

### Timeframe

I first spent multiple weeks doing light research occasionally, unsure how to approach the task correctly.
I got to know the basics of Envoy, set it up and drafted some planned implementation.  
When I started implementing the cache, I created most of the code over 2-3 days.  
Then I wrote the tests and debugged it over around 1 day.

### Sources used

I used these sources to learn about the architecture of Envoy:
- [Envoy docs](https://www.envoyproxy.io/docs/envoy/v1.35.0/)
- [Envoy source markdown docs](https://github.com/envoyproxy/envoy/tree/main/source/docs)
- Envoy source code, particularly `Buffer::OwnedImpl` and the existing HTTP cache extension
- [Envoy Proxy Insider](https://envoy-insider.mygraphql.com/en/latest/index.html)
- various medium and other blog posts