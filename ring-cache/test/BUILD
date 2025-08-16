load("@envoy//bazel:envoy_build_system.bzl", "envoy_cc_test")

envoy_cc_test(
    name = "unit_test",
    srcs = ["ring_cache_unit_test.cc"],
    repository = "@envoy",
    deps = [
        "//ring-cache:ring_cache_lib",
        "@envoy//source/common/buffer:buffer_lib",
        "@envoy//source/common/http:header_map_lib",
        "@envoy//test/mocks/http:http_mocks",
        "@envoy//test/test_common:simulated_time_system_lib",
        "@envoy//test/test_common:utility_lib",
    ],
)

envoy_cc_test(
    name = "integration_test",
    srcs = ["ring_cache_integration_test.cc"],
    repository = "@envoy",
    deps = [
        "//ring-cache:ring_cache_lib",
        "@envoy//test/integration:http_integration_lib",
        "@envoy//test/test_common:utility_lib",
    ],
)
