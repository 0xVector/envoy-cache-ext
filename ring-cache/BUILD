load(
    "@envoy//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_proto_library",
)
load("@envoy_api//bazel:api_build_system.bzl", "api_proto_package")

api_proto_package()
#envoy_extension_package()

#proto_library(
#    name = "cache_proto",
#    srcs = ["cache.proto"],
#    deps = ["@envoy_api//envoy/config/core/v3:pkg",
##            "@com_envoyproxy_protoc_gen_validate//:validate_proto"
#            ],
#)

#envoy_proto_library(
#    name = "cache_proto_cc",
#    srcs  = ["cache.proto"],
#    deps  = [
#        "@envoy_api//envoy/config/core/v3:pkg",
##        "@com_envoyproxy_protoc_gen_validate//:validate_proto",
#    ],
##    proto_library = ":cache_proto",
#)

envoy_cc_library(
    name = "ring_cache_lib",
    srcs = glob(["*.cc"]),
    hdrs = glob(["*.h"]),
    visibility = ["//visibility:public"],  # allow parents to depend on it
    deps = [
        ":pkg_cc_proto",
        #        ":cache_proto_cc",
        "@envoy//source/common/buffer:buffer_lib",
        "@envoy//source/common/common:base_logger_lib",  # Logger
        "@envoy//source/common/http:filter_manager_lib",  # for filter.h, might be stale
        "@envoy//source/common/http:header_map_lib",
        "@envoy//source/common/protobuf:utility_lib",
        #        "@envoy//source/common/stats:stats_lib",
        "@envoy//source/common/thread_local:thread_local_lib",
    ],
)
