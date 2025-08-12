#pragma once
#include "envoy/http/filter.h"
#include "config.h"
#include "cache.h"

namespace Envoy::Extensions::HttpFilters::RingCache {
    class RingCacheFilterDecoder : public Http::StreamDecoderFilter,
                                   public Logger::Loggable<Logger::Id::filter> {
    public:
        explicit RingCacheFilterDecoder(RingCacheFilterConfigSharedPtr);
        ~RingCacheFilterDecoder() override;

        // Http::StreamFilterBase
        void onDestroy() override;

        // Http::StreamDecoderFilter
        Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers,
                                                bool end_stream) override;
        Http::FilterDataStatus decodeData(Buffer::Instance& data, bool end_stream) override;
        Http::FilterTrailersStatus decodeTrailers(Http::RequestTrailerMap& trailers) override;
        void setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) override;

    private:
        enum class Role { Unset, Cached, Leader, Follower };

        static constexpr absl::string_view Separator = "///";
        const RingCacheFilterConfigSharedPtr config_;
        RingBufferCacheSharedPtr cache_;
        Http::StreamDecoderFilterCallbacks* decoder_callbacks_;
        std::string key_;
        Role role_{Role::Unset};

        bool build_key(absl::string_view host, absl::string_view path);
    };
}
