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
# Should get the whole body immediatelly because it was cached previously
```

To try out the eviction behaviour live, try setting the `capacity` cache config param in [`cache_filter.yaml`](cache_filter.yaml) config file
to a low value (in bytes) and request multiple different paths to fill the cache. The oldest (most usually) entry should
then get evicted and will again have to be fetched from the upstream (which will try to re-cache it).

### Config

The cache uses a standard protobuf config with 2 configurable parameters:
- `capacity` - cumulative limit on cached response size in bytes
- `slot_count` - limit on the number of distinct entries in the cache

These can be set in the YAML config file.

## Architecture overview

The whole source is in the [`ring-cache`](ring-cache) directory.

### Filter

The filter is implemented in the [`filter.cc`](ring-cache/filter.cc) as a class implementing the `Http::StreamFilter`
interface, thus it is both an encoder and a decoder.

The filter instances and request streams are grouped into 3 distinct categories - roles:
- Hit (when a response is fully cached -> can be handed out immediately)
- Follower (when a response is not cached, but is inflight -> can be coalesced)
- Leader (when a response is not cached and not inflight -> an upstream request has to be made)

The filter is meant to be lean and offload most functionality to the `RingBufferCache` class. It tracks the basic
state of its stream and uses the `RingBufferCache::lookup` function to determine the caching status for a request.
This happens when decoding downstream request headers, ignoring the request body, trailers and metadata.  
On upstream response encoding, it is responsible for passing the headers / body data back to the cache so it can be
cached and handed out to followers that could be coalesced. The filter (in a leader role) is then left to use the
upstream response for itself. Other filter roles are not meant to receive a response directly from the upstream.

### Cache

The cache itself is implemented in the `RingBufferCache` class in [cache.cc](ring-cache/cache.cc). It provides a simple
public API that the filter can use to utilize the caching functionality.

The cache uses bounded memory to store responses which can be se by the `capacity` parameter.
It doesn't take into account the inflight coalesced responses at this point, which is a limitation that could be
removed in the future, at the cost of some added complexity.

#### Data structures

The cache uses these internal data structures:
- fixed sized (`slot_count` config param) ring buffer of unique pointers to cache entries
- hash map of keys -> cache entries for fast lookup
- hash map of keys -> inflight entries

All of these structures are guarded by a single internal mutex.

**Rationale:** My interpretation of the ring buffer cache from the task specification is that of a fixed size buffer of pointers to entries.
I picked this implementation because of several reasons:
- it allows for simple eviction / free slot allocation via a single straightforward scan
- cache hit serving is fast, as only a no-copy `BufferFragment` is constructed from the existing entry
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
  takes ownership of the `HeaderMap` and thus needs it's own copy.  
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

### Cache API overview

`lookup(key, callbacks)` - is called as the first stage of every new request stream. It atomically determines the outcome
for this key and returns a result with the appropriate category (`Hit/Follower/Leader`, see above). `Hit` results also
contain the cached data and can immediately be used to fulfill the request, whereas `Follower/Leader` results are more
like a marker instructing the filter to wait for further processing.

`publishHeaders(key, headers, end_stream)` - function for publishing the request headers by a `Leader`. This
triggers the backfilling of coalesced `Followers` and can also finalize the response if it's the end of the stream,
moving it to the permanent cache.

`publishData(key, data, end_stream)` - similar to the function above but for publishing request data. Has to only be called
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
