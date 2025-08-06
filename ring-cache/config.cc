#include "envoy/server/filter_config.h"
#include "config.h"
#include "filter.h"
#include "proto-includes.h"

namespace Envoy::Extensions::HttpFilters::RingCache {
    class RingCacheFilterFactory : public Server::Configuration::NamedHttpFilterConfigFactory {
    public:
        Http::FilterFactoryCb createFilterFactoryFromProtoWithServerContext(
            const Protobuf::Message& proto, const std::string&,
            Server::Configuration::ServerFactoryContext& ctx) override {
            const auto& config = MessageUtil::downcastAndValidate<const ProtoRingCacheFilterConfig &>(
                proto, ctx.messageValidationVisitor());

            auto shared_cfg = std::make_shared<RingCacheFilterConfig>(config);

            return [shared_cfg](Http::FilterChainFactoryCallbacks& callbacks) -> void {
                callbacks.addStreamDecoderFilter(std::make_shared<RingCacheFilterDecoder>(shared_cfg));
            };
        }

        absl::StatusOr<Http::FilterFactoryCb> createFilterFactoryFromProto(
            const Protobuf::Message& proto, const std::string& str,
            Server::Configuration::FactoryContext& ctx)
        override {
            return createFilterFactoryFromProtoWithServerContext(proto, str, ctx.serverFactoryContext());
        }

        ProtobufTypes::MessagePtr createEmptyConfigProto() override {
            return std::make_unique<ProtoRingCacheFilterConfig>();
        }

        [[nodiscard]] std::string name() const override { return "envoy.filters.http.ring_cache"; }
    };

    REGISTER_FACTORY(RingCacheFilterFactory, Server::Configuration::NamedHttpFilterConfigFactory);
}
