load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

cc_library(
    name = "utf8",
    srcs = ["utf8.cpp"],
    hdrs = ["utf8.hpp"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "utf8_test",
    size = "small",
    srcs = ["utf8_test.cpp"],
    deps = [
        ":utf8",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
