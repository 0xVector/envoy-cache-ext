#pragma once
#include "cache.h"
#include "proto-includes.h"
#include "envoy/server/factory_context.h"

namespace Envoy::Extensions::HttpFilters::RingCache {
    class RingCacheFilterConfig {
    public:
        explicit RingCacheFilterConfig(const ProtoRingCacheFilterConfig& proto,
                                       Server::Configuration::ServerFactoryContext& ctx);

        [[nodiscard]] uint32_t cacheSize() const { return cache_size_; }
        [[nodiscard]] uint64_t slotCount() const { return slot_count_; }
        [[nodiscard]] RingBufferCacheSharedPtr cache() const { return cache_; }

        // Stats::Scope& scope();

    private:
        uint64_t cache_size_;
        uint64_t slot_count_;

    private:
        RingBufferCacheSharedPtr cache_;
        // Stats::Scope& scope_;
    };

    using RingCacheFilterConfigSharedPtr = std::shared_ptr<const RingCacheFilterConfig>;
}
