#include "cache.h"

#include <atomic>
#include "source/common/http/header_map_impl.h"

namespace Envoy::Extensions::HttpFilters::RingCache {
    RingBufferCache::RingBufferCache(const size_t capacity, const size_t slot_count) : capacity_(capacity),
        slot_count_(slot_count) {
        slots_.resize(slot_count);
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
                Entry* entry = cached_it->second;
                entry->pins_.fetch_add(1, std::memory_order_relaxed); // Increase in use to avoid eviction
                header_ptr = entry->headers_.get();
                body_data_ptr = entry->body_.data();
                body_length = entry->body_.length();
                pin_ptr = &entry->pins_;
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
                    attachBackfillWaiterLocked(inflight_it->second, result.waiter_);
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

            if (end_stream) { finalizeLocked(key); }
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

            if (end_stream) { finalizeLocked(key); }
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

    void RingBufferCache::finalizeLocked(const key_t& key) { // TODO: pass inflight_it as arg to avoid look up again ?
        const auto inflight_it = inflight_map_.find(key);
        ASSERT(inflight_it != inflight_map_.end(), "Finalize called on non-inflight key");
        Inflight& inflight = inflight_it->second;

        // Have space to move to permanent cache
        const size_t size = inflight.size() + key.size();
        if (evictTillCapacityLocked(size)) {
            // Insert
            slots_[head_] = std::make_unique<Entry>();
            Entry& slot = *slots_[head_];
            slot.key_ = key;
            slot.headers_ = std::move(inflight.headers_);
            slot.body_ = std::move(inflight.body_);

            cache_map_.insert_or_assign(key, slots_[head_].get());
            used_size_ += size;
            ASSERT(size == slot.size());
        }

        inflight_map_.erase(inflight_it);
    }

    void RingBufferCache::attachBackfillWaiterLocked(Inflight& inflight, const WaiterSharedPtr& waiter) {
        inflight.waiters_.push_back(waiter);

        // Presume not the end of stream - if it were end, this would not be inflight anymore
        ASSERT(inflight.headers_ || inflight.body_.empty());

        // Headers ready
        Http::ResponseHeaderMapPtr header;
        if (inflight.headers_) {
            header = Http::createHeaderMap<Http::ResponseHeaderMapImpl>(*inflight.headers_);
        } else {
            ASSERT(inflight.body_.empty());
            return;
        }

        // Body ready
        Buffer::OwnedImpl buffer;
        if (!inflight.body_.empty()) { buffer.add(inflight.body_); } // Copy out whole prefix

        waiter->dispatcher_->post( // TODO: buffer is a big struct so expensive copy, rework?
            [w=waiter, h=std::move(header), b=std::move(buffer)]() mutable {
                if (w->cancelled.load(std::memory_order_acquire)) { return; }
                w->callbacks_->encodeHeaders(std::move(h), false, RingCacheDetailsMessageCoalescedBackfill);
                if (b.length() > 0) {
                    w->callbacks_->encodeData(b, false);
                }
            });
    }

    bool RingBufferCache::evictTillCapacityLocked(const size_t size_needed) {
        const size_t start = head_;
        while (capacity_ - used_size_ < size_needed) {
            head_ = (head_ + 1) % slot_count_;

            // Back at start
            if (start == head_) { return false; }

            // Try evict
            if (!slots_[head_]) { continue; }
            Entry& entry = *slots_[head_];
            if (entry.pins_.load(std::memory_order_acquire) > 0) { continue; }

            // Can evict
            used_size_ -= entry.size();
            cache_map_.erase(entry.key_);
            slots_[head_].reset();
        }
        ASSERT(!slots_[head_]);
        return true;
    }
}
