load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

cc_library(
    name = "timer",
    srcs = [
        "poller.cpp",
        "scheduler.cpp",
    ],
    hdrs = [
        "deadline.hpp",
        "poller.hpp",
        "scheduler.hpp",
        "status.hpp",
        "timer.hpp",
    ],
    visibility = ["//visibility:public"],
    deps = ["//utils/multithreading"],
)

cc_test(
    name = "timer_test",
    size = "small",
    srcs = ["timer_test.cpp"],
    deps = [
        ":timer",
        "//utils/multithreading",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
