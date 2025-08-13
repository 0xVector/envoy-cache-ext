#pragma once
#include "envoy/event/dispatcher.h"
#include "envoy/http/filter.h"
#include "envoy/http/header_map.h"
#include <memory>
#include <string>
#include <vector>
#include <optional>

#include "envoy/singleton/instance.h"

namespace Envoy::Extensions::HttpFilters::RingCache {
    constexpr absl::string_view RingCacheDetailsMessageHit ="ring_cache.hit";
    constexpr absl::string_view RingCacheDetailsMessageCoalesced = "ring_cache.coalesced";

    class RingBufferCache : public Singleton::Instance, public Logger::Loggable<Logger::Id::filter> {
    private:
        absl::Mutex mutex_; // protects: cache_map_, inflight_map_, slots_
        // invariant: waiters are fully backfilled
        // invariant: lookup() is resolved to exactly 1 type

    public:
        struct Hit {
            Http::ResponseHeaderMapPtr headers_;
            std::shared_ptr<const std::string> body_;
        };

        enum class ResultType { Hit, Leader, Follower };

        struct LookupResult {
            ResultType type_{};
            std::optional<Hit> hit_;
        };

        struct Waiter {
            Event::Dispatcher* dispatcher_;
            Http::StreamDecoderFilterCallbacks* callbacks_;
        };

        using key_t = std::string;

        explicit RingBufferCache(uint64_t capacity);
        [[nodiscard]] LookupResult lookup(const key_t& key, const Waiter& waiter) ABSL_LOCKS_EXCLUDED(mutex_);
        void publishHeaders(const key_t& key, const Http::ResponseHeaderMap& response_headers, bool end_stream)
        ABSL_LOCKS_EXCLUDED(mutex_); // Should only be called by the leader
        void publishData(const key_t& key, const Buffer::Instance& data, bool end_stream) ABSL_LOCKS_EXCLUDED(mutex_);
        // Should only be called by the leader

    private:
        struct Entry {
            Http::ResponseHeaderMapPtr headers_;
            // ResponseMetadata metadata_;
            std::shared_ptr<const std::string> body_;
            // Http::ResponseTrailerMapPtr trailers_;
            std::string key_;

            [[nodiscard]] uint64_t length() const;
        };

        struct Inflight {
            Http::ResponseHeaderMapPtr headers_;
            std::string body_;
            std::vector<Waiter> waiters_;
        };

        const uint64_t capacity_;

        uint64_t used_size_ = 0;
        absl::flat_hash_map<key_t, Entry> cache_map_ ABSL_GUARDED_BY(mutex_);
        absl::flat_hash_map<key_t, Inflight> inflight_map_ ABSL_GUARDED_BY(mutex_);
        std::vector<absl::optional<Entry> > slots_ ABSL_GUARDED_BY(mutex_);

        void finalize(const key_t& key); // must flush existing waiters (and feed them)
        void attach_waiter_locked(Inflight& inflight, const Waiter& waiter) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
        void evictTillCapacity(uint64_t size_needed);
    };

    SINGLETON_MANAGER_REGISTRATION(ring_cache_singleton); // Register name for the singleton

    using RingBufferCacheSharedPtr = std::shared_ptr<RingBufferCache>;
}
