#pragma once
#include "envoy/http/filter.h"

namespace Envoy::Extensions::HttpFilters::RingCache {
    class RingCacheFilterConfig; // Forward declaration
    using RingCacheFilterConfigSharedPtr = std::shared_ptr<const RingCacheFilterConfig>;

    class RingCacheFilterDecoder : public Http::StreamDecoderFilter, public Logger::Loggable<Logger::Id::filter> {
    public:
        explicit RingCacheFilterDecoder(RingCacheFilterConfigSharedPtr);
        ~RingCacheFilterDecoder() override;

        // Http::StreamFilterBase
        void onDestroy() override;

        // Http::StreamDecoderFilter
        Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers, bool end_stream) override;
        Http::FilterDataStatus decodeData(Buffer::Instance& data, bool end_stream) override;
        Http::FilterTrailersStatus decodeTrailers(Http::RequestTrailerMap& trailers) override;
        void setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) override;

    private:
        const RingCacheFilterConfigSharedPtr config_;
        Http::StreamDecoderFilterCallbacks* decoder_callbacks_;

        // const LowerCaseString headerKey() const;
        // const std::string headerValue() const;
    };
}
