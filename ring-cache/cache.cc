#include "cache.h"

#include <atomic>
#include "source/common/http/header_map_impl.h"

namespace Envoy::Extensions::HttpFilters::RingCache {
    RingBufferCache::RingBufferCache(const uint64_t capacity) : capacity_(capacity) {
        slots_.resize(capacity);
    }

    RingBufferCache::LookupResult RingBufferCache::lookup(const key_t& key,
                                                          Http::StreamDecoderFilterCallbacks* callbacks) {
        LookupResult result;
        const Http::ResponseHeaderMap* header_ptr = nullptr;
        const char* body_data_ptr = nullptr;
        std::atomic<uint32_t>* pin_ptr = nullptr;
        size_t body_length = 0;

        /* Lock */ {
            absl::MutexLock lock(&mutex_);

            const auto cached_it = cache_map_.find(key);

            // Hit
            if (cached_it != cache_map_.end()) {
                result.type_ = ResultType::Hit;
                auto& entry = cached_it->second;
                entry.pins_.fetch_add(1, std::memory_order_relaxed); // Increase in use to avoid eviction
                header_ptr = entry.headers_.get();
                body_data_ptr = entry.body_.data();
                body_length = entry.body_.length();
                pin_ptr = &entry.pins_;
            }

            // Lead/Follow
            else {
                const auto [inflight_it, inserted] = inflight_map_.try_emplace(key);
                // Become leader
                if (inserted) {
                    result.type_ = ResultType::Leader;
                }

                // Inflight Hit (Follower)
                else {
                    result.type_ = ResultType::Follower;
                    result.waiter_ = std::make_shared<Waiter>(&callbacks->dispatcher(), callbacks);
                    inflight_it->second.waiters_.push_back(result.waiter_); // Add a shared_ptr copy to waiters
                }

                return result;
            }
        }

        // Serve hit
        Hit hit;
        hit.headers_ = Http::createHeaderMap<Http::ResponseHeaderMapImpl>(*header_ptr);
        if (body_length == 0) {
            pin_ptr->fetch_sub(1, std::memory_order_release);
        } else {
            auto* fragment = new Buffer::BufferFragmentImpl(body_data_ptr, body_length,
                                                            [p=pin_ptr](
                                                        const void*, size_t, const Buffer::BufferFragmentImpl* self) {
                                                                p->fetch_sub(1, std::memory_order_release);
                                                                delete self;
                                                            });
            hit.body_.addBufferFragment(*fragment);
        }

        result.hit_ = std::move(hit);
        return result;
    }

    void RingBufferCache::publishHeaders(const key_t& key, const Http::ResponseHeaderMap& response_headers,
                                         bool end_stream) {
        std::vector<WaiterSharedPtr> waiters_copy;
        Http::ResponseHeaderMapPtr header_copy;

        /* Lock */ {
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
        for (const auto& waiter: waiters_copy) {
            auto new_header_copy = Http::createHeaderMap<Http::ResponseHeaderMapImpl>(*header_copy);
            waiter->dispatcher_->post(
                [w=waiter, h=std::move(new_header_copy), end=end_stream]() mutable {
                    if (w->cancelled.load(std::memory_order_acquire)) { return; }
                    w->callbacks_->encodeHeaders(std::move(h), end, RingCacheDetailsMessageCoalesced);
                });
            // TODO: dispatcher isThreadSafe() ??
        }

        if (end_stream) { finalize(key); }
    }

    void RingBufferCache::publishData(const key_t& key, const Buffer::Instance& data, bool end_stream) {
        std::vector<WaiterSharedPtr> waiters_copy;

        /* Lock */ {
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
            waiter->dispatcher_->post([w=waiter, c=std::move(c), end=end_stream]() mutable {
                if (w->cancelled.load(std::memory_order_acquire)) { return; }
                auto* fragment = new Buffer::BufferFragmentImpl(c->data(), c->size(),
                                                                [c=std::move(c)](
                                                            const void*, size_t,
                                                            const Buffer::BufferFragmentImpl* self) mutable {
                                                                    c.reset();
                                                                    delete self;
                                                                });
                Buffer::OwnedImpl buffer;
                buffer.addBufferFragment(*fragment);
                w->callbacks_->encodeData(buffer, end);
            });
        }
        // TODO: waiter could possibly get freed before post is finished

        if (end_stream) { finalize(key); }
    }

    void RingBufferCache::removeWaiter(const key_t& key, const WaiterSharedPtr& waiter) {
        absl::MutexLock lock(&mutex_);

        const auto it = inflight_map_.find(key);
        if (it == inflight_map_.end()) {
            ENVOY_LOG(debug, "[CACHE] removeWaiter: inflight entry missing");
            return;
        }

        Inflight& inflight = it->second;
        std::vector<WaiterSharedPtr>& waiters = inflight.waiters_;

        for (size_t i = 0; i < waiters.size(); ++i) { // TODO: O(1)
            if (waiters[i] == waiter) { // Swap & pop (comparing ptrs)
                std::swap(waiters[i], waiters[waiters.size() - 1]);
                waiters.pop_back();
                return;
            }
        }

        ENVOY_LOG(debug, "[CACHE] removeWaiter: waiter entry missing");
    }

    void RingBufferCache::finalize(const key_t& key) {
        std::vector<Waiter> waiters_copy;

        /* Lock */ {
            absl::MutexLock lock(&mutex_);

            const auto it = inflight_map_.find(key);
            ASSERT(it != inflight_map_.end(), "Finalize called on not-inflight key");
        }
    }
}
