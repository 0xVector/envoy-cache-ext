#include "gtest/gtest.h"
#include "gmock/gmock.h"

#include "ring-cache/cache.h"
#include "source/common/http/header_map_impl.h"
#include "source/common/buffer/buffer_impl.h"

#include "source/common/api/api_impl.h"
#include "source/common/stats/isolated_store_impl.h"
#include "test/mocks/http/mocks.h"
#include "test/test_common/simulated_time_system.h"

using ::testing::_;
using ::testing::Invoke;
using Envoy::Buffer::OwnedImpl;
using Envoy::Http::ResponseHeaderMapImpl;

namespace Envoy::Extensions::HttpFilters::RingCache {
    struct Record {
        std::vector<std::string> chunks;
        bool saw_headers = false;
        bool end_stream_from_headers = false;
        bool end_stream_from_data = false;
    };

    class CacheLogicTest : public testing::Test {
    };

    TEST(CacheLogicTest, BecomeLeader) {
        RingBufferCache cache(1024, 64);

        auto res = cache.lookup("k", nullptr);

        EXPECT_EQ(res.type_, RingBufferCache::ResultType::Leader);
        EXPECT_FALSE(res.hit_.has_value());
        EXPECT_EQ(res.waiter_, nullptr);
    }

    TEST(CacheLogicTest, HitAfterFinalize) {
        RingBufferCache cache(1024, 64);

        // Leader fills inflight and finalizes
        auto hdrs = ResponseHeaderMapImpl::create();
        hdrs->setStatus(200);
        OwnedImpl d1("test");

        auto leader = cache.lookup("k", nullptr);
        cache.publishHeaders("k", *hdrs, false);
        cache.publishData("k", d1, true);

        auto res = cache.lookup("k", nullptr);

        EXPECT_EQ(res.type_, RingBufferCache::ResultType::Hit);
        EXPECT_TRUE(res.hit_.has_value());
        auto& [resp_headers, resp_body] = res.hit_.value();

        EXPECT_EQ(resp_headers->getStatusValue(), "200");
        EXPECT_EQ(resp_body.toString(), "test");
    }

    TEST(CacheLogicTest, NoHitNewKey) {
        RingBufferCache cache(1024, 64);

        auto res = cache.lookup("k", nullptr);
        auto res2 = cache.lookup("k2", nullptr);

        EXPECT_EQ(res2.type_, RingBufferCache::ResultType::Leader);
        EXPECT_FALSE(res2.hit_.has_value());
        EXPECT_EQ(res2.waiter_, nullptr);
    }

    TEST(CacheLogicTest, FollowerBackfillLiveAndEnd) {
        Stats::IsolatedStoreImpl stats;
        Event::SimulatedTimeSystem time_system;
        auto api = Api::createApiForTest(stats, time_system);
        Event::DispatcherPtr dispatcher = api->allocateDispatcher("test");

        RingBufferCache cache(1024, 64);
        constexpr absl::string_view key = "a.test\0/slow";

        // Leader publish without end_stream
        auto leader = cache.lookup(key, nullptr);
        auto hdrs = ResponseHeaderMapImpl::create();
        hdrs->setStatus(200);
        cache.publishHeaders(key, *hdrs, false);

        // Mock decoder callbacks for the follower
        NiceMock<Http::MockStreamDecoderFilterCallbacks> cb;
        ON_CALL(cb, dispatcher()).WillByDefault(::testing::ReturnRef(*dispatcher));

        Record cap;
        ON_CALL(cb, encodeHeaders_(_, _))
                .WillByDefault(Invoke([&](Http::ResponseHeaderMap&, const bool end) {
                    cap.saw_headers = true;
                    cap.end_stream_from_headers = end;
                }));
        ON_CALL(cb, encodeData(_, _))
                .WillByDefault(Invoke([&](const Buffer::Instance& b, const bool end) {
                    cap.chunks.push_back(b.toString());
                    cap.end_stream_from_data = end;
                }));

        // Follower attaches
        auto follower = cache.lookup(key, &cb);
        ASSERT_EQ(follower.type_, RingBufferCache::ResultType::Follower);

        // Leader publishes a data chunk (not end)
        OwnedImpl chunk1("chunk-1\n");
        cache.publishData(key, chunk1, false);

        // Allow posted backfill/live callbacks to run
        dispatcher->run(Event::Dispatcher::RunType::NonBlock);

        // Follower should have received headers once and the first chunk (not ended)
        EXPECT_TRUE(cap.saw_headers);
        ASSERT_EQ(cap.chunks.size(), 1);
        EXPECT_EQ(cap.chunks[0], "chunk-1\n");
        EXPECT_FALSE(cap.end_stream_from_headers);
        EXPECT_FALSE(cap.end_stream_from_data);

        // Leader ends the stream with an empty buffer
        OwnedImpl empty;
        cache.publishData(key, empty, true);

        dispatcher->run(Event::Dispatcher::RunType::NonBlock);

        // Follower must observe EOS (end_stream true on last encodeData)
        EXPECT_TRUE(cap.end_stream_from_data);
        EXPECT_FALSE(cap.end_stream_from_headers);

        // Subsequent lookup should be a cache hit with full body
        auto hit = cache.lookup(key, nullptr);
        ASSERT_EQ(hit.type_, RingBufferCache::ResultType::Hit);
        auto& [resp_headers2, resp_body2] = hit.hit_.value();
        EXPECT_EQ(resp_headers2->getStatusValue(), "200");
        EXPECT_EQ(resp_body2.toString(), "chunk-1\n");
    }
}
