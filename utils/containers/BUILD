load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

cc_library(
    name = "trie",
    hdrs = ["trie.hpp"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "trie_test",
    size = "small",
    srcs = ["trie_test.cpp"],
    deps = [
        ":trie",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
