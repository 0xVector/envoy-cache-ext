#include "cache.h"

#include <atomic>
#include "source/common/http/header_map_impl.h"

namespace Envoy::Extensions::HttpFilters::RingCache {
    RingBufferCache::RingBufferCache(const size_t capacity, const size_t slot_count) : capacity_(capacity),
        slot_count_(slot_count) {
        slots_.resize(slot_count);
    }

    RingBufferCache::LookupResult RingBufferCache::lookup(const absl::string_view key,
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
                // Copy out data from cache
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
                    ASSERT(callbacks != nullptr);
                    result.waiter_ = std::make_shared<Waiter>(&callbacks->dispatcher(), callbacks);
                    attachBackfillWaiterLocked(inflight_it->second, result.waiter_);
                }

                return result;
            }
        }

        // Serve hit - unlocked
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

    void RingBufferCache::publishHeaders(const absl::string_view key, const Http::ResponseHeaderMap& response_headers,
                                         bool end_stream) {
        std::vector<WaiterSharedPtr> waiters_copy;
        Http::ResponseHeaderMapPtr header_copy;
        ENVOY_LOG(debug, "[CACHE] publishHeaders: key={}, end_stream={}", key, end_stream);

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
        ENVOY_LOG(debug, "[CACHE] publishHeaders: feeding {} waiters", waiters_copy.size());
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

    void RingBufferCache::publishData(const absl::string_view key, const Buffer::Instance& data, bool end_stream) {
        std::vector<WaiterSharedPtr> waiters_copy;
        ENVOY_LOG(debug, "[CACHE] publishData: key={}, end_stream={}", key, end_stream);

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
        ENVOY_LOG(debug, "[CACHE] publishData: feeding {} waiters", waiters_copy.size());
        for (auto& waiter: waiters_copy) {
            auto c = chunk; // Bump refcount
            waiter->dispatcher_->post([w=waiter, c=std::move(c), end=end_stream]() mutable {
                if (w->cancelled.load(std::memory_order_acquire)) { return; }
                auto* fragment = new Buffer::BufferFragmentImpl(c->data(), c->size(),
                                                                [c=std::move(c)](
                                                            const void*, size_t,
                                                            const Buffer::BufferFragmentImpl* self) mutable noexcept {
                                                                    c.reset();
                                                                    delete self;
                                                                });
                Buffer::OwnedImpl buffer;
                buffer.addBufferFragment(*fragment);
                w->callbacks_->encodeData(buffer, end);
            });
        }
    }

    void RingBufferCache::removeWaiter(const absl::string_view key, const WaiterSharedPtr& waiter) {
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

    void RingBufferCache::finalizeLocked(const absl::string_view key) { // TODO: pass inflight_it as arg to avoid look up again ?
        const auto inflight_it = inflight_map_.find(key);
        ASSERT(inflight_it != inflight_map_.end(), "Finalize called on non-inflight key");
        Inflight& inflight = inflight_it->second;
        ENVOY_LOG(debug, "[CACHE] finalizeLocked: key={}", key);

        // Have space to move to permanent cache
        const size_t size = inflight.size() + key.size();
        if (evictTillCapacityLocked(size)) {
            insertEntryLocked(key, std::move(inflight.headers_), std::move(inflight.body_));
            ENVOY_LOG(debug, "[CACHE] finalizeLocked: key={} moved to cache, size={}", key, size);
        } else { ENVOY_LOG(debug, "[CACHE] finalizeLocked: key={} dropped due to no space", key); }

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

        ENVOY_LOG(debug, "[CACHE] backfilling waiter: key={}, headers={}, body_length={}, inflight_size={}",
                  inflight.headers_->getStatusValue(), header->size(), buffer.length(), inflight.size());

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
        ENVOY_LOG(debug, "[CACHE] starting eviction: size_needed={}, used_size_={}, capacity_={}",
                  size_needed, used_size_, capacity_);
        if (size_needed > capacity_) { // Early exit when exceeding total capacity
            ENVOY_LOG(debug, "[CACHE] eviction failed: size_needed={} > capacity_={}", size_needed, capacity_);
            return false;
        }

        const size_t start = head_;
        // Head might be at the just inserted-to slot now

        // Ensure enough capacity
        while (capacity_ - used_size_ < size_needed) {
            // Try evict
            if (slots_[head_]) {
                Entry& entry = *slots_[head_];
                if (entry.pins_.load(std::memory_order_acquire) == 0) { // Can evict
                    evictHeadLocked();
                }
            }

            head_ = (head_ + 1) % slot_count_; // Advance head

            // Back at start, if not enough space we have lost
            if (start == head_ && capacity_ - used_size_ < size_needed) {
                ENVOY_LOG(debug, "[CACHE] no space: size_needed={}", size_needed);
                return false;
            }
        }

        // Might need to also find a free slot if we haven't evicted
        const size_t start_slot = head_;
        while (slots_[head_]) {
            head_ = (head_ + 1) % slot_count_;
            if (head_ == start_slot) { return false; } // TODO evict first non-pinned just for the slot
        }

        ASSERT(!slots_[head_]);
        ENVOY_LOG(debug, "[CACHE] eviction done: size_needed={}, used_size_={}, capacity_={}",
                  size_needed, used_size_, capacity_);
        return true;
    }

    void RingBufferCache::evictHeadLocked() {
        ASSERT(slots_[head_], "Head slot should be occupied before eviction");
        Entry& entry = *slots_[head_];
        ENVOY_LOG(debug, "[CACHE] evicted: key={}, size={}", entry.key_, entry.size());
        used_size_ -= entry.size();
        cache_map_.erase(entry.key_);
        slots_[head_].reset();
    }

    void RingBufferCache::insertEntryLocked(absl::string_view key, Http::ResponseHeaderMapPtr&& headers,
        std::string&& body) {
        ASSERT(!slots_[head_], "Head slot should be empty before insert");
        slots_[head_] = std::make_unique<Entry>();
        Entry& slot = *slots_[head_];
        slot.key_ = key;
        slot.headers_ = std::move(headers);
        slot.body_ = std::move(body);

        cache_map_.insert_or_assign(key, slots_[head_].get());
        head_ = (head_ + 1) % slot_count_; // Advance head
        used_size_ += slot.size();
    }
}
