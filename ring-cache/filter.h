#pragma once
#include "envoy/http/filter.h"
#include "config.h"
#include "cache.h"

namespace Envoy::Extensions::HttpFilters::RingCache {
    class RingCacheFilterDecoder :
            public Http::StreamFilter,
            public Logger::Loggable<Logger::Id::filter> {
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

        // Http::StreamEncoderFilter
        Http::Filter1xxHeadersStatus encode1xxHeaders(Http::ResponseHeaderMap& headers) override;
        Http::FilterHeadersStatus encodeHeaders(Http::ResponseHeaderMap& headers, bool end_stream) override;
        Http::FilterDataStatus encodeData(Buffer::Instance& data, bool end_stream) override;
        Http::FilterTrailersStatus encodeTrailers(Http::ResponseTrailerMap& trailers) override;
        Http::FilterMetadataStatus encodeMetadata(Http::MetadataMap& metadata_map) override;
        void setEncoderFilterCallbacks(Http::StreamEncoderFilterCallbacks& callbacks) override;

    private:
        enum class Role { Unset, Cached, Leader, Follower };

        static constexpr absl::string_view Separator = "///";
        const RingCacheFilterConfigSharedPtr config_;
        RingBufferCacheSharedPtr cache_;
        Http::StreamDecoderFilterCallbacks* decoder_callbacks_{nullptr};
        Http::StreamEncoderFilterCallbacks* encoder_callbacks_{nullptr};
        std::string key_;
        Role role_{Role::Unset};

        bool buildKey(absl::string_view host, absl::string_view path);
    };
}
