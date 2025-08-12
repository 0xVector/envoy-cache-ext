#pragma once
#include "cache.h"
#include "proto-includes.h"
#include "envoy/server/factory_context.h"

namespace Envoy::Extensions::HttpFilters::RingCache {
    class RingCacheFilterConfig {
    public:
        explicit RingCacheFilterConfig(const ProtoRingCacheFilterConfig& proto,
                                       Server::Configuration::ServerFactoryContext& ctx);

        uint32_t cacheSize() const { return cache_size_; }

        RingBufferCacheSharedPtr cache() const { return cache_; }

        // Stats::Scope& scope();

    private:
        uint32_t cache_size_;
        RingBufferCacheSharedPtr cache_;
        // Stats::Scope& scope_;
    };

    using RingCacheFilterConfigSharedPtr = std::shared_ptr<const RingCacheFilterConfig>;
}
