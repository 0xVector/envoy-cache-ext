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
    constexpr absl::string_view RingCacheDetailsMessageHit = "ring_cache.hit";
    constexpr absl::string_view RingCacheDetailsMessageCoalesced = "ring_cache.coalesced";
    constexpr absl::string_view RingCacheDetailsMessageCoalescedBackfill = "ring_cache.coalesced_backfill";

    class RingBufferCache : public Singleton::Instance, public Logger::Loggable<Logger::Id::filter> {
    private:
        absl::Mutex mutex_; // protects: cache_map_, inflight_map_, slots_
        // invariant: waiters are fully backfilled
        // invariant: lookup() is resolved to exactly 1 type

    public:
        struct Hit {
            Http::ResponseHeaderMapPtr headers_;
            Buffer::OwnedImpl body_;
        };

        enum class ResultType { Hit, Leader, Follower };

        struct Waiter {
            Event::Dispatcher* dispatcher_;
            Http::StreamDecoderFilterCallbacks* callbacks_;
            std::atomic<bool> cancelled{false};
            // TODO: store index in waiters for O(1) removal
        };
        using WaiterSharedPtr = std::shared_ptr<Waiter>;

        struct LookupResult {
            ResultType type_{};
            std::optional<Hit> hit_; // Only set when type_ == Hit
            WaiterSharedPtr waiter_; // Only not null when type_ == Follower
        };

        using key_t = std::string;

        explicit RingBufferCache(uint64_t capacity);
        [[nodiscard]] LookupResult lookup(const key_t& key, Http::StreamDecoderFilterCallbacks* callbacks) ABSL_LOCKS_EXCLUDED(mutex_);
        void publishHeaders(const key_t& key, const Http::ResponseHeaderMap& response_headers, bool end_stream)
        ABSL_LOCKS_EXCLUDED(mutex_); // Should only be called by the leader
        void publishData(const key_t& key, const Buffer::Instance& data, bool end_stream) ABSL_LOCKS_EXCLUDED(mutex_);
        // Should only be called by the leader
        void removeWaiter(const key_t& key, const WaiterSharedPtr& waiter);

    private:
        struct Entry {
            std::string key_; // Needed for eviction
            Http::ResponseHeaderMapPtr headers_;
            // ResponseMetadata metadata_;
            std::string body_;
            // Http::ResponseTrailerMapPtr trailers_;
            std::atomic<uint32_t> pins_{0}; // Eviction guard (# of uses)

            Entry(const Entry&) = delete;
            Entry(Entry&&) = delete;
            ~Entry() { ASSERT(pins_.load(std::memory_order_relaxed) == 0); }
            [[nodiscard]] uint64_t length() const;
        };

        struct Inflight {
            Http::ResponseHeaderMapPtr headers_;
            std::string body_;
            std::vector<WaiterSharedPtr> waiters_;
        };

        const uint64_t capacity_;

        uint64_t used_size_ = 0;
        absl::flat_hash_map<key_t, Entry> cache_map_ ABSL_GUARDED_BY(mutex_);
        absl::flat_hash_map<key_t, Inflight> inflight_map_ ABSL_GUARDED_BY(mutex_);
        std::vector<std::unique_ptr<Entry> > slots_ ABSL_GUARDED_BY(mutex_);
        // Must never reallocate - entries must stay in place

        // must flush existing waiters (and feed them)
        void finalize(const key_t& key) ABSL_LOCKS_EXCLUDED(mutex_);
        static void attach_backfill_waiter_locked(Inflight& inflight, const WaiterSharedPtr& waiter) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
        void evictTillCapacity(uint64_t size_needed);
    };

    SINGLETON_MANAGER_REGISTRATION(ring_cache_singleton); // Register name for the singleton

    using RingBufferCacheSharedPtr = std::shared_ptr<RingBufferCache>;
}
