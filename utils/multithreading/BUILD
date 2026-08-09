load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

cc_library(
    name = "multithreading",
    srcs = ["job_queue.cpp"],
    hdrs = [
        "job_queue.hpp",
        "multithreading.hpp",
        "status.hpp",
    ],
    visibility = ["//visibility:public"],
    deps = ["//utils/logger"],
)

exports_files(["README.md"])

cc_test(
    name = "multithreading_test",
    size = "small",
    srcs = ["job_queue_test.cpp"],
    deps = [
        ":multithreading",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
