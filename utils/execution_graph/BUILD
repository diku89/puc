load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

exports_files(["README.md"])

cc_library(
    name = "dependency_graph",
    hdrs = [
        "dependency_graph.hpp",
        "status.hpp",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "execution_graph",
    srcs = ["execution_graph.cpp"],
    hdrs = ["execution_graph.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":dependency_graph",
        "//utils/logger",
        "//utils/multithreading",
    ],
)

cc_test(
    name = "dependency_graph_test",
    size = "small",
    srcs = ["dependency_graph_test.cpp"],
    deps = [
        ":dependency_graph",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "execution_graph_test",
    size = "small",
    srcs = ["execution_graph_test.cpp"],
    deps = [
        ":execution_graph",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
