#pragma once
#include "ring-cache/cache.pb.h"
#include "ring-cache/cache.pb.validate.h"

namespace Envoy::Extensions::HttpFilters::RingCache {
    using ProtoRingCacheFilterConfig = envoy::extensions::filters::http::ring_cache::v3::RingCacheFilterConfig;
}