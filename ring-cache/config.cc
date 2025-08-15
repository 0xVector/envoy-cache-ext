#include "config.h"

#include "cache.h"
#include "envoy/server/filter_config.h"
#include "filter.h"
#include "proto-includes.h"

namespace Envoy::Extensions::HttpFilters::RingCache {
    SINGLETON_MANAGER_REGISTRATION(ring_cache_singleton); // Register name for the singleton

    RingCacheFilterConfig::RingCacheFilterConfig(const ProtoRingCacheFilterConfig& proto,
                                                 Server::Configuration::ServerFactoryContext& ctx) : cache_size_(
        proto.ring_size()), slot_count_(proto.slot_count()) {
        cache_ = ctx.singletonManager().getTyped<RingBufferCache>(
            SINGLETON_MANAGER_REGISTERED_NAME(ring_cache_singleton),
            //std::string(SingletonCacheName),
            [&proto] {
                return std::make_shared<RingBufferCache>(proto.ring_size(), proto.slot_count());
            }, true);
    }

    class RingCacheFilterFactory : public Server::Configuration::NamedHttpFilterConfigFactory {
    public:
        Http::FilterFactoryCb createFilterFactoryFromProtoWithServerContext(
            const Protobuf::Message& proto, const std::string&,
            Server::Configuration::ServerFactoryContext& ctx) override {
            const auto& parsed_proto = MessageUtil::downcastAndValidate<const ProtoRingCacheFilterConfig &>(
                proto, ctx.messageValidationVisitor());

            auto shared_cfg = std::make_shared<RingCacheFilterConfig>(parsed_proto, ctx);

            return [shared_cfg](Http::FilterChainFactoryCallbacks& callbacks) -> void {
                callbacks.addStreamFilter(std::make_shared<RingCacheFilter>(shared_cfg));
            };
        }

        absl::StatusOr<Http::FilterFactoryCb>
        createFilterFactoryFromProto(const Protobuf::Message& proto, const std::string& str,
                                     Server::Configuration::FactoryContext& ctx) override {
            return createFilterFactoryFromProtoWithServerContext(proto, str, ctx.serverFactoryContext());
        }

        ProtobufTypes::MessagePtr createEmptyConfigProto() override {
            return std::make_unique<ProtoRingCacheFilterConfig>();
        }

        [[nodiscard]] std::string name() const override { return "envoy.filters.http.ring_cache"; }
    };

    REGISTER_FACTORY(RingCacheFilterFactory, Server::Configuration::NamedHttpFilterConfigFactory);
}
