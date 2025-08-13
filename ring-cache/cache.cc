#include "cache.h"

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

    void RingBufferCache::publishData(const key_t& key, const Buffer::Instance& data, bool end_stream) {
        std::vector<Waiter> waiters_copy;

        // Lock
        {
            absl::MutexLock lock(&mutex_);

            const auto it = inflight_map_.find(key);
            if (it == inflight_map_.end()) {
                ENVOY_LOG(debug, "[CACHE] publishData: inflight entry missing");
                return;
            }

            auto& [_, body, waiters] = it->second;

            // Copy out new data chunk to inflight spool
            const size_t old_size = body.length();
            body.resize(body.length() + data.length());
            data.copyOut(0, data.length(), body.data() + old_size); // Copy out whole data to the end of body

            waiters_copy = waiters; // Snapshot waiters
        }

        // Copy data to be used for all the coalesced waiters
        const auto chunk = std::make_shared<std::string>(data.toString());

        // Feed possible waiters - can be done unlocked (waiters coming after unlock will be fed by attach from already present headers)
        for (auto& waiter: waiters_copy) {
            auto c = chunk; // Bump refcount
            waiter.dispatcher_->post([cb=waiter.callbacks_, c=std::move(c), end=end_stream]() mutable {
                Buffer::BufferFragmentImpl fragment(c->data(), c->size(),
                                                    [c=std::move(c)](auto, auto, auto) mutable { c.reset(); });
                Buffer::OwnedImpl buffer;
                buffer.addBufferFragment(fragment);
                cb->encodeData(buffer, end);
            });
        }
        // TODO: waiter could possibly get freed before post is finished

        if (end_stream) { finalize(key); }
    }
}
