#include "test/integration/http_integration.h"

namespace Envoy {
    class RingCacheFilterIntegrationTest : public HttpIntegrationTest,
                                           public testing::TestWithParam<Network::Address::IpVersion> {
    public:
        RingCacheFilterIntegrationTest() : HttpIntegrationTest(Http::CodecClient::Type::HTTP1, GetParam()) {}

        void initializeWithFilter() {
            config_helper_.addFilter(
                "{ name: ring_cache, "
                "typed_config: { \"@type\": type.googleapis.com/envoy.extensions.filters.http.ring_cache.v3.RingCacheFilterConfig, "
                "capacity: 128, slot_count: 128 } }");

            // Generous timeouts so chunking/slow upstream doesn't reset streams
            config_helper_.addConfigModifier([&](ConfigHelper::HttpConnectionManager& hcm) {
                hcm.mutable_common_http_protocol_options()->mutable_idle_timeout()->set_seconds(300);
                hcm.mutable_route_config()->mutable_virtual_hosts(0)->mutable_routes(0)
                        ->mutable_route()->mutable_timeout()->set_seconds(300);
            });

            initialize();
        }

        void SetUp() override { initializeWithFilter(); }
    };

    INSTANTIATE_TEST_SUITE_P(IpVersions, RingCacheFilterIntegrationTest,
                             testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

    TEST_P(RingCacheFilterIntegrationTest, BasicCacheHit) {
        // Create a downstream request to a path that is not cached
        auto client1 = makeHttpConnection(makeClientConnection(lookupPort("http")));
        auto ed1 = client1->startRequest(Http::TestRequestHeaderMapImpl{
            {":method", "GET"}, {":path", "/x"}, {":authority", "a.test"}
        });

        // Wait for the upstream connection to be established
        ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*dispatcher_, fake_upstream_connection_));
        FakeStreamPtr upstream;
        ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream));

        // Send response from upstream
        Buffer::OwnedImpl buff("hello");
        upstream->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, false);
        upstream->encodeData(buff, true);

        // Wait for the downstream response to be received
        auto& resp1 = std::get<1>(ed1);
        ASSERT_TRUE(resp1->waitForEndStream());
        EXPECT_EQ(resp1->body(), "hello");

        // Ensure the upstream request was made
        EXPECT_EQ(test_server_->counter("cluster.cluster_0.upstream_rq_total")->value(), 1);

        // Send a second request to the same path - this should hit the cache
        auto client2 = makeHttpConnection(makeClientConnection(lookupPort("http")));
        auto ed2 = client2->startRequest(Http::TestRequestHeaderMapImpl{
            {":method", "GET"}, {":path", "/x"}, {":authority", "a.test"}
        });
        auto& resp2 = std::get<1>(ed2);
        ASSERT_TRUE(resp2->waitForEndStream());
        EXPECT_EQ(resp2->body(), "hello");

        // Verify that the second request did not result in an upstream request
        EXPECT_EQ(test_server_->counter("cluster.cluster_0.upstream_rq_total")->value(), 1);

        ASSERT_TRUE(fake_upstream_connection_->waitForDisconnect());
        client1->close();
        EXPECT_TRUE(client1->waitForDisconnect());
        client2->close();
        EXPECT_TRUE(client2->waitForDisconnect());
    }

    TEST_P(RingCacheFilterIntegrationTest, LateFollowerGetsCoalesced) {
        // Leader request
        auto client1 = makeHttpConnection(makeClientConnection(lookupPort("http")));
        auto ed1 = client1->startRequest(Http::TestRequestHeaderMapImpl{
            {":method", "GET"}, {":path", "/slow"}, {":authority", "a.test"}
        });
        auto& resp1 = std::get<1>(ed1);

        // Wait for the upstream connection to be established
        ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*dispatcher_, fake_upstream_connection_));
        FakeStreamPtr upstream;
        ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream));

        // Upstream sends headers + first chunk (not the end)
        Buffer::OwnedImpl chunk1("chunk-1\n");
        upstream->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, false);
        upstream->encodeData(chunk1, false);

        // Late follower requests the same path
        auto client2 = makeHttpConnection(makeClientConnection(lookupPort("http")));
        auto ed2 = client2->startRequest(Http::TestRequestHeaderMapImpl{
            {":method", "GET"}, {":path", "/slow"}, {":authority", "a.test"}
        });

        // Follower should receive the first chunk immediately
        auto& resp2 = std::get<1>(ed2);
        resp2->waitForBodyData(8);
        EXPECT_EQ(resp2->body(), "chunk-1\n");

        // Send more live chunk, both A and B should receive them
        Buffer::OwnedImpl chunk2("chunk-2\n");
        Buffer::OwnedImpl chunk3("chunk-3\n");
        upstream->encodeData(chunk2, false);
        upstream->encodeData(chunk3, true);

        // Wait for both responses to complete
        ASSERT_TRUE(resp1->waitForEndStream());
        ASSERT_TRUE(resp2->waitForEndStream());

        // Expect both responses to have the same body
        EXPECT_EQ(resp1->body(), "chunk-1\nchunk-2\nchunk-3\n");
        EXPECT_EQ(resp2->body(), "chunk-1\nchunk-2\nchunk-3\n");

        // Assert only 1 upstream request was made
        EXPECT_EQ(test_server_->counter("cluster.cluster_0.upstream_rq_total")->value(), 1);

        client1->close();
        EXPECT_TRUE(client1->waitForDisconnect());
        client2->close();
        EXPECT_TRUE(client2->waitForDisconnect());
    }

    TEST_P(RingCacheFilterIntegrationTest, NoCapacityLeftCausesEviction) {
        constexpr size_t number_requests = 4;
        std::string big_data(30, 'x');
        Buffer::OwnedImpl big_chunk(big_data);

        // Fill the cache with requests to different paths
        for (size_t i = 0; i < number_requests; ++i) {
            // Leader request
            auto client1 = makeHttpConnection(makeClientConnection(lookupPort("http")));
            auto ed1 = client1->startRequest(Http::TestRequestHeaderMapImpl{
                {":method", "GET"}, {":path", absl::StrCat("/", i)}, {":authority", "a.test"}
            });
            auto& resp1 = std::get<1>(ed1);

            // Wait for the upstream connection to be established
            ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*dispatcher_, fake_upstream_connection_));
            FakeStreamPtr upstream;
            ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream));

            // Upstream sends big response
            upstream->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, false);
            upstream->encodeData(big_chunk, true);

            // Check the response
            ASSERT_TRUE(resp1->waitForEndStream());
            EXPECT_EQ(resp1->body(), big_data);
            client1->close();
            EXPECT_TRUE(client1->waitForDisconnect());

            // Expect cached
            auto client2 = makeHttpConnection(makeClientConnection(lookupPort("http")));
            auto ed2 = client2->startRequest(Http::TestRequestHeaderMapImpl{
                {":method", "GET"}, {":path", absl::StrCat("/", i)}, {":authority", "a.test"}
            });
            auto& resp2 = std::get<1>(ed2);
            ASSERT_TRUE(resp2->waitForEndStream());
            EXPECT_EQ(resp2->body(), big_data);
            client2->close();
            EXPECT_TRUE(client2->waitForDisconnect());

            // Only one upstream request should have been made
            EXPECT_EQ(test_server_->counter("cluster.cluster_0.upstream_rq_total")->value(), i+1);
        }

        // Now we check that the first cached response was evicted

        // New downstream request to the first path
        auto client = makeHttpConnection(makeClientConnection(lookupPort("http")));
        auto ed = client->startRequest(Http::TestRequestHeaderMapImpl{
            {":method", "GET"}, {":path", "/0"}, {":authority", "a.test"}
        });
        auto& resp = std::get<1>(ed);

        // Upstream
        ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*dispatcher_, fake_upstream_connection_));
        FakeStreamPtr upstream;
        ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream));
        upstream->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, false);
        upstream->encodeData(big_chunk, true);

        // Check the response
        ASSERT_TRUE(resp->waitForEndStream());
        EXPECT_EQ(resp->body(), big_data);
        client->close();
        EXPECT_TRUE(client->waitForDisconnect());

        // One extra upstream request should have been made to fetch the evicted response
        EXPECT_EQ(test_server_->counter("cluster.cluster_0.upstream_rq_total")->value(), number_requests + 1);
    }
}
