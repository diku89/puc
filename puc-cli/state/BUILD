load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

cc_library(
    name = "state",
    srcs = ["state.cpp"],
    hdrs = [
        "lifecycle.hpp",
        "state.hpp",
    ],
    visibility = ["//visibility:public"],
    deps = ["//utils/execution_graph:dependency_graph"],
)

cc_test(
    name = "state_test",
    size = "small",
    srcs = ["state_test.cpp"],
    deps = [
        ":state",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
