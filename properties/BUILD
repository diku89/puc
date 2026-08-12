load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

cc_library(
    name = "properties",
    srcs = ["properties.cpp"],
    hdrs = ["properties.hpp"],
    visibility = ["//visibility:public"],
    deps = ["//utils/config"],
)

cc_test(
    name = "properties_test",
    size = "small",
    srcs = ["properties_test.cpp"],
    deps = [
        ":properties",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
