#include "ring_cache_filter.h"
#include "envoy/server/filter_config.h"
#include "ring-cache/cache.pb.h"

namespace Envoy::Extensions::HttpFilters::RingCache {
    class RingCacheFilterConfig {
    public:
        RingCacheFilterConfig() = default;

        // Stats::Scope& scope();

    private:
        // Stats::Scope& scope_;
    };

    class RingCacheFilterFactory : public Server::Configuration::NamedHttpFilterConfigFactory {
    public:
        absl::StatusOr<Http::FilterFactoryCb> createFilterFactoryFromProto(
            const Protobuf::Message&, const std::string&,
            Server::Configuration::FactoryContext&) override {

            auto shared_cfg = std::make_shared<RingCacheFilterConfig>();

            return [shared_cfg](Http::FilterChainFactoryCallbacks& callbacks) -> void {
                callbacks.addStreamDecoderFilter(std::make_shared<RingCacheFilterDecoder>(shared_cfg));
            };
        }

        ProtobufTypes::MessagePtr createEmptyConfigProto() override {
            return std::make_unique<envoy::extensions::filters::http::ring_cache::v3::RingCacheFilterConfig>();
        }

        [[nodiscard]] std::string name() const override { return "envoy.filters.http.ring_cache"; }
    };

    REGISTER_FACTORY(RingCacheFilterFactory, Server::Configuration::NamedHttpFilterConfigFactory);
}
