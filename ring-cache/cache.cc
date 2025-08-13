#include "cache.h"

#include "../../../../.cache/bazel/_bazel_vector/79390a7724858d3d4ef648cd638a14af/external/com_google_cel_cpp/common/optional_ref.h"
#include "source/common/http/header_map_impl.h"

namespace Envoy::Extensions::HttpFilters::RingCache {
    RingBufferCache::RingBufferCache(const uint64_t capacity) : capacity_(capacity) {
        slots_.resize(capacity);
    }

    RingBufferCache::LookupResult RingBufferCache::lookup(const key_t& key, const Waiter& waiter) {
        absl::MutexLock lock(&mutex_);

        LookupResult result;

        // Hit
        const auto cached_it = cache_map_.find(key);
        if (cached_it != cache_map_.end()) {
            auto& [headers, body, _] = cached_it->second;
            result.type_ = ResultType::Hit;
            result.hit_.emplace(Http::createHeaderMap<Http::ResponseHeaderMapImpl>(*headers), body);
            return result;
        }

        const auto [inflight_it, inserted] = inflight_map_.try_emplace(key);
        // Become leader
        if (inserted) {
            result.type_ = ResultType::Leader;
        }

        // Inflight Hit
        else {
            result.type_ = ResultType::Follower;
            attach_waiter_locked(inflight_it->second, waiter);
        }

        return result;
    }

    void RingBufferCache::publishHeaders(const key_t& key, const Http::ResponseHeaderMap& response_headers,
                                         bool end_stream) {
        std::vector<Waiter> waiters_copy;
        Http::ResponseHeaderMapPtr header_copy;

        // Lock
        {
            absl::MutexLock lock(&mutex_);

            const auto it = inflight_map_.find(key);
            if (it == inflight_map_.end()) {
                ENVOY_LOG(debug, "[CACHE] publishHeaders: inflight entry missing");
                return;
            }

            Inflight& inflight = it->second;
            if (!inflight.headers_) {
                inflight.headers_ = Http::createHeaderMap<Http::ResponseHeaderMapImpl>(response_headers);
            }

            waiters_copy = inflight.waiters_; // Snapshot waiters
            header_copy = Http::createHeaderMap<Http::ResponseHeaderMapImpl>(*inflight.headers_); // Snapshot headers
        }

        // Feed possible waiters - can be done unlocked (waiters coming after unlock will be fed by attach from already present headers)
        for (auto& waiter: waiters_copy) {
            auto new_header_copy = Http::createHeaderMap<Http::ResponseHeaderMapImpl>(*header_copy);
            waiter.dispatcher_->post([cb=waiter.callbacks_, h=std::move(new_header_copy), end=end_stream]() mutable {
                cb->encodeHeaders(std::move(h), end, RingCacheDetailsMessageCoalesced);
            });
        }

        if (end_stream) { finalize(key); }
    }
}
