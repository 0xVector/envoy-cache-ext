#include "filter.h"
#include <memory>

namespace Envoy::Extensions::HttpFilters::RingCache {
    RingCacheFilter::RingCacheFilter(RingCacheFilterConfigSharedPtr config) : config_(std::move(config)),
        cache_(config_->cache()) {}

    RingCacheFilter::~RingCacheFilter() = default;

    void RingCacheFilter::onDestroy() {
        if (role_ == Role::Follower && !done_) {
            waiter_->cancelled.store(true, std::memory_order_release);
            cache_->removeWaiter(key_, waiter_);
        }
        // TODO: cancellation for Leader !!!
    }

    Http::FilterHeadersStatus RingCacheFilter::decodeHeaders(Http::RequestHeaderMap& headers, const bool) {
        ENVOY_LOG(debug, "[CACHE] decodeHeaders for {}{} rs={}", headers.getHostValue(),
                  headers.getPathValue(), config_->cacheSize());

        if (!buildKey(headers.getHostValue(), headers.getPathValue())) {
            ENVOY_LOG(debug, "[CACHE] decodeHeaders: host or path is empty, skipping cache");
            return Http::FilterHeadersStatus::Continue;
        }

        auto [type, hit, waiter] = cache_->lookup(key_, decoder_callbacks_);

        switch (type) {
            case RingBufferCache::ResultType::Hit: {
                role_ = Role::Cached;
                ENVOY_LOG(debug, "[CACHE] decodeHeaders: cache hit for key={}", key_);
                auto& [resp_headers, resp_body] = hit.value();
                const bool has_body = resp_body.length() > 0;
                decoder_callbacks_->encodeHeaders(std::move(resp_headers), !has_body, RingCacheDetailsMessageHit);
                if (has_body) {
                    ENVOY_LOG(debug, "[CACHE] decodeHeaders: sending cached body for key={}", key_);
                    decoder_callbacks_->encodeData(resp_body, true);
                }
                done_ = true;
                ENVOY_LOG(debug, "[CACHE] decodeHeaders: done for key={}", key_);
                return Http::FilterHeadersStatus::StopIteration;
            }

            case RingBufferCache::ResultType::Leader: {
                role_ = Role::Leader;
                ENVOY_LOG(debug, "[CACHE] decodeHeaders: cache leader for key={}", key_);
                return Http::FilterHeadersStatus::Continue;
            }

            case RingBufferCache::ResultType::Follower: {
                role_ = Role::Follower;
                waiter_ = waiter;
                ENVOY_LOG(debug, "[CACHE] decodeHeaders: cache follower for key={}", key_);
                return Http::FilterHeadersStatus::StopIteration;
            }
        }

        return Http::FilterHeadersStatus::Continue;
    }

    Http::FilterDataStatus RingCacheFilter::decodeData(Buffer::Instance&, bool) {
        return Http::FilterDataStatus::Continue;
    }

    Http::FilterTrailersStatus RingCacheFilter::decodeTrailers(Http::RequestTrailerMap&) {
        return Http::FilterTrailersStatus::Continue;
    }

    void RingCacheFilter::setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) {
        decoder_callbacks_ = &callbacks;
    }

    Http::Filter1xxHeadersStatus RingCacheFilter::encode1xxHeaders(Http::ResponseHeaderMap&) {
        return Http::Filter1xxHeadersStatus::Continue;
    }

    Http::FilterHeadersStatus
    RingCacheFilter::encodeHeaders(Http::ResponseHeaderMap& headers, const bool end_stream) {
        ENVOY_LOG(debug, "[CACHE] encodeHeaders for key {} end_stream={}", key_, end_stream);
        if (end_stream) { done_ = true; }
        if (role_ != Role::Leader) { return Http::FilterHeadersStatus::Continue; }
        cache_->publishHeaders(key_, headers, end_stream);
        return Http::FilterHeadersStatus::Continue;
    }

    Http::FilterDataStatus RingCacheFilter::encodeData(Buffer::Instance& data, const bool end_stream) {
        ENVOY_LOG(debug, "[CACHE] encodeData for key {} end_stream={}", key_, end_stream);
        if (end_stream) { done_ = true; }
        if (role_ != Role::Leader) { return Http::FilterDataStatus::Continue; }
        cache_->publishData(key_, data, end_stream);
        return Http::FilterDataStatus::Continue;
    }

    Http::FilterTrailersStatus RingCacheFilter::encodeTrailers(Http::ResponseTrailerMap&) {
        ENVOY_LOG(debug, "[CACHE] got trailers !!!");
        return Http::FilterTrailersStatus::Continue;
    }

    Http::FilterMetadataStatus RingCacheFilter::encodeMetadata(Http::MetadataMap&) {
        ENVOY_LOG(debug, "[CACHE] got metadata !!!");
        return Http::FilterMetadataStatus::Continue;
    }

    void RingCacheFilter::setEncoderFilterCallbacks(Http::StreamEncoderFilterCallbacks& callbacks) {
        encoder_callbacks_ = &callbacks;
    }

    bool RingCacheFilter::buildKey(const absl::string_view host, const absl::string_view path) {
        if (host.empty() || path.empty())
            return false;

        key_.reserve(host.size() + path.size() + 1);
        key_.append(host);
        key_.append(Separator);
        key_.append(path);
        return true;
    }
}
