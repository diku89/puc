load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

cc_library(
    name = "address",
    srcs = ["address.cpp"],
    hdrs = ["address.hpp"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "turn_tree",
    srcs = ["turn_tree.cpp"],
    hdrs = ["turn_tree.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":address",
        "//canvas/protos:canvas_cc_proto",
        "//utils/containers:trie",
        "//utils/hash",
    ],
)

cc_test(
    name = "address_test",
    size = "small",
    srcs = ["address_test.cpp"],
    deps = [
        ":address",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
