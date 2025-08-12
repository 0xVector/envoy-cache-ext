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
    class RingBufferCache : public Singleton::Instance {
    private:
        struct Entry {
            Http::ResponseHeaderMapPtr response_headers_;
            // ResponseMetadata metadata_;
            std::shared_ptr<const std::string> body_;
            // Http::ResponseTrailerMapPtr trailers_;
            std::string key_;

            [[nodiscard]] uint64_t length() const;
        };

    public:
        struct Hit {
            Http::ResponseHeaderMapPtr headers_;
            std::shared_ptr<const std::string> body_;
        };

        enum class ResultType { Hit, Leader, Follower };

        struct LookupResult {
            ResultType type_;
            std::optional<Hit> hit_;
        };

        struct Waiter {
            Event::Dispatcher* dispatcher_;
            Http::StreamDecoderFilterCallbacks* callbacks_;
        };

        using key_t = std::string;

        explicit RingBufferCache(uint64_t capacity);
        [[nodiscard]] LookupResult lookup(const key_t& key, const Waiter& waiter) const;
        bool insert(const key_t& key, Http::ResponseHeaderMapPtr&& response_headers, std::string&& body);

    private:
        const uint64_t capacity_;
        uint64_t used_size_ = 0;
        // std::deque<key_t> queue_;
        absl::flat_hash_map<key_t, Entry> map_;
        std::vector<std::unique_ptr<Entry> > ring_buffer_;
        absl::Mutex mutex_;

        void evict_till_capacity(uint64_t size_needed);
    };

    SINGLETON_MANAGER_REGISTRATION(ring_cache_singleton); // Register name for the singleton

    using RingBufferCacheSharedPtr = std::shared_ptr<RingBufferCache>;
}
