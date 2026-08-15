load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

cc_library(
    name = "hash",
    srcs = ["sha256.cpp"],
    hdrs = ["sha256.hpp"],
    visibility = ["//visibility:public"],
    deps = ["@mbedtls"],
)

cc_test(
    name = "hash_test",
    size = "small",
    srcs = ["sha256_test.cpp"],
    deps = [
        ":hash",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
