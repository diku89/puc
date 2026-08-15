load("@protobuf//bazel:cc_proto_library.bzl", "cc_proto_library")
load("@protobuf//bazel:proto_library.bzl", "proto_library")

proto_library(
    name = "canvas_proto",
    srcs = [
        "actor.proto",
        "canvas.proto",
        "channels.proto",
        "presentation.proto",
        "turn.proto",
    ],
    visibility = ["//visibility:public"],
)

cc_proto_library(
    name = "canvas_cc_proto",
    visibility = ["//visibility:public"],
    deps = [":canvas_proto"],
)
