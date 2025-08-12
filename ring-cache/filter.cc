#include "filter.h"
#include <memory>

namespace Envoy::Extensions::HttpFilters::RingCache {
    RingCacheFilterDecoder::RingCacheFilterDecoder(RingCacheFilterConfigSharedPtr config) : config_(std::move(config)),
        cache_(config->cache()), decoder_callbacks_(nullptr) {}

    RingCacheFilterDecoder::~RingCacheFilterDecoder() = default;
    void RingCacheFilterDecoder::onDestroy() {} // TODO: drop own waiter if follower

    Http::FilterHeadersStatus RingCacheFilterDecoder::decodeHeaders(Http::RequestHeaderMap& headers,
                                                                    const bool end_stream) {
        ENVOY_LOG(debug, "[CACHE] decodeHeaders for {}{} rs={}", headers.getHostValue(),
                  headers.getPathValue(), config_->cacheSize());

        if (!build_key(headers.getHostValue(), headers.getPathValue())) {
            ENVOY_LOG(debug, "[CACHE] decodeHeaders: host or path is empty, skipping cache");
            return Http::FilterHeadersStatus::Continue;
        }

        auto lookup_result = cache_->lookup(
            key_, RingBufferCache::Waiter{&decoder_callbacks_->dispatcher(), decoder_callbacks_});

        switch (lookup_result.type_) {
            case RingBufferCache::ResultType::Hit: {
                role_ = Role::Cached;
                ENVOY_LOG(debug, "[CACHE] decodeHeaders: cache hit for key={}", key_);
                auto& [resp_headers, resp_body] = lookup_result.hit_.value();
                decoder_callbacks_->encodeHeaders(std::move(resp_headers),
                                                  end_stream, //! resp_body || resp_body->empty(),
                                                  "ring_cache.hit");
                if (resp_body && !resp_body->empty()) { // !endstream
                    ENVOY_LOG(debug, "[CACHE] decodeHeaders: sending cached body for key={}", key_);
                    Buffer::OwnedImpl out(resp_body->data()); // Copies data to Buffer
                    decoder_callbacks_->encodeData(out, true);
                }
                return Http::FilterHeadersStatus::StopIteration;
            }

            case RingBufferCache::ResultType::Leader: {
                role_ = Role::Leader;
                ENVOY_LOG(debug, "[CACHE] decodeHeaders: cache leader for key={}", key_);
                // decoder_callbacks_->streamInfo().setResponseFlag(StreamInfo::CoreResponseFlag::ResponseFromCacheFilter);
                // decoder_callbacks_->streamInfo().setResponseCodeDetails("ring_cache.leader");
                return Http::FilterHeadersStatus::Continue;
            }

            case RingBufferCache::ResultType::Follower: {
                role_ = Role::Follower;
                ENVOY_LOG(debug, "[CACHE] decodeHeaders: cache follower for key={}", key_);
                return Http::FilterHeadersStatus::StopIteration;
            }
        }

        return Http::FilterHeadersStatus::Continue;
    }

    Http::FilterDataStatus RingCacheFilterDecoder::decodeData(Buffer::Instance&, bool) {
        return Http::FilterDataStatus::Continue;
    }

    Http::FilterTrailersStatus RingCacheFilterDecoder::decodeTrailers(Http::RequestTrailerMap&) {
        return Http::FilterTrailersStatus::Continue;
    }

    void RingCacheFilterDecoder::setDecoderFilterCallbacks(
        Http::StreamDecoderFilterCallbacks& callbacks) {
        decoder_callbacks_ = &callbacks;
    }

    bool RingCacheFilterDecoder::build_key(const absl::string_view host, const absl::string_view path) {
        if (host.empty() || path.empty())
            return false;

        key_.reserve(host.size() + path.size() + 1);
        key_.append(host);
        key_.append(Separator);
        key_.append(path);
        return true;
    }
}
