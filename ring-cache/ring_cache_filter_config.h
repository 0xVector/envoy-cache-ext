#pragma once
#include "proto-includes.h"

namespace Envoy::Extensions::HttpFilters::RingCache {
    class RingCacheFilterConfig {
    public:
        explicit RingCacheFilterConfig(const ProtoRingCacheFilterConfig& config):
            ring_size_(config.ring_size()) {}

        uint32_t ringSize() const { return ring_size_; }

        // Stats::Scope& scope();

    private:
        uint32_t ring_size_;
        // Stats::Scope& scope_;
    };

    using RingCacheFilterConfigSharedPtr = std::shared_ptr<const RingCacheFilterConfig>;
}
