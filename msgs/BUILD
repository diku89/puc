load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

cc_library(
    name = "msgs",
    srcs = [
        "codec.cpp",
        "screen_msgs.cpp",
    ],
    hdrs = [
        "codec.hpp",
        "msgs.hpp",
        "null_message.hpp",
        "screen_msgs.hpp",
        "status.hpp",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//utils/ipc",
        "//utils/logger",
    ],
)

exports_files(["README.md"])

cc_test(
    name = "msgs_test",
    size = "small",
    srcs = [
        "codec_test.cpp",
        "screen_msgs_test.cpp",
    ],
    deps = [
        ":msgs",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
