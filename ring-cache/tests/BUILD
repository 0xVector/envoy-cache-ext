load("@envoy//bazel:envoy_build_system.bzl", "envoy_cc_test")

envoy_cc_test(
    name = "ring_cache_unit_test",
    srcs = ["ring_cache_test.cc"],
    deps = [
        "//ring-cache:ring_cache_lib",
        "@envoy//test/test_common:envoy_test_main",
    ],
)
