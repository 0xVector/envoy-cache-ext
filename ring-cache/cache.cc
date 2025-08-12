#include "cache.h"

namespace Envoy::Extensions::HttpFilters::RingCache {
    RingBufferCache::RingBufferCache(const uint64_t capacity) : capacity_(capacity) {
        ring_buffer_.resize(capacity);
    }
}
