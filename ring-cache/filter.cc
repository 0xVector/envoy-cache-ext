#include "filter.h"
#include <memory>

namespace Envoy::Extensions::HttpFilters::RingCache {
    RingCacheFilterDecoder::RingCacheFilterDecoder(RingCacheFilterConfigSharedPtr config):
        config_(std::move(config)), decoder_callbacks_(nullptr) {}

    RingCacheFilterDecoder::~RingCacheFilterDecoder() = default;
    void RingCacheFilterDecoder::onDestroy() {}

    Http::FilterHeadersStatus RingCacheFilterDecoder::decodeHeaders(Http::RequestHeaderMap& headers, bool) {//end_stream
        ENVOY_LOG(debug, "[CACHE] decodeHeaders for {}{} rs={}", headers.getHostValue(),
                  headers.getPathValue(), config_->ringSize());

        return Http::FilterHeadersStatus::Continue;
    }

    Http::FilterDataStatus RingCacheFilterDecoder::decodeData(Buffer::Instance&, bool) {//data
        return Http::FilterDataStatus::Continue;
    }

    Http::FilterTrailersStatus RingCacheFilterDecoder::decodeTrailers(Http::RequestTrailerMap&) {
        return Http::FilterTrailersStatus::Continue;
    }

    void RingCacheFilterDecoder::setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) {
        decoder_callbacks_ = &callbacks;
    }
}
