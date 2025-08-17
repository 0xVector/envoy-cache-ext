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

## Config

The cache uses a standard protobuf config with 2 configurable parameters:
- `capacity` - cumulative limit on cached response size in bytes
- `slot_count` - limit on the number of distinct entries in the cache

These can be set in the YAML config file.

## Code overview

The whole source is in the [`ring-cache`](ring-cache) directory.

### Design

#### Overview

The filter instances and request streams are grouped into 3 distinct categories:
- Hit (when a response is fully cached -> can be handed out immediately)
- Follower (when a response is not cached, but is inflight -> can be coalesced)
- Leader (when a response is not cached and not inflight -> an upstream request has to be made)

#### Filter

The filter is implemented in the [`filter.cc`](ring-cache/filter.cc) as a class implementing the `Http::StreamFilter`
interface, thus it is both an encoder and a decoder.  
The filter is meant to be leaner and offload most functionality to the `RingBufferCache` class. It tracks the basic
state of its stream and uses the `RingBufferCache::lookup` function to determine the caching status for a request.
This happens when decoding downstream request headers, ignoring the request body, trailers and metadata.  
On upstream response encoding, it is responsible for passing the headers / body data back to the cache so it can be
cached and handed out to followers that could be coalesced. The filter (in a leader role) is then left to use the
upstream response for itself. Other filter roles are not meant to receive a response directly from the upstream.

#### Cache

The cache itself is implemented in the `RingBufferCache` class in [cache.cc](ring-cache/cache.cc). It provides a simple
public API that the filter can use to utilize the caching functionality.

##### API overview

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

##### Thread safety

The cache class public API is thread safe. The thread-safety is built around a single mutex and some atomics.

##### Data structures

The cache uses these internal data structures:
- fixed sized (`slot_count` config param) ring buffer of unique pointers to cache entries
- hash map of keys -> cache entries for fast lookup
- hash map of keys -> inflight entries

All of these structures are guarded by the single internal mutex.

#### Config

The protobuf config is loaded and parsed by the `RingCacheFilterConfig` class in [`config.cc`](ring-cache/config.cc).

### Design rationale

#### Multi-threading

The decision to use just a single mutex is due to several reasons:

1. Apart from data structure operations, the mutex also guards internal invariants. Splitting the mutex to several
finer-grained would break or severely complicate this. An example is that waiting followers are always fully backfilled
when the mutex is not held - this greatly simplifies the backfilling process, and allowing it to be guarded by a separate
mutex to e.g. the one guarding `lookup()` would easily violate it. Many of the invariants are interlinked like this and
hard to separate to multiple mutexes.
2. Complexity. Multiple mutexes increase design complexity and care must be taken to avoid deadlocks.

Considering these, I found it reasonable to use just a single mutex at this stage while trying to limit critical section size.