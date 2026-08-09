load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

cc_library(
    name = "metronome",
    srcs = ["metronome.cpp"],
    hdrs = [
        "metronome.hpp",
        "status.hpp",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//msgs",
        "//utils/ipc",
        "//utils/logger",
        "//utils/multithreading",
    ],
)

exports_files(["README.md"])

cc_test(
    name = "metronome_test",
    size = "small",
    srcs = ["metronome_test.cpp"],
    deps = [
        ":metronome",
        "//msgs",
        "//utils/ipc",
        "//utils/multithreading",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
