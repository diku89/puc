load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

cc_library(
    name = "msgs",
    srcs = [
        "cmdframe_msgs.cpp",
        "codec.cpp",
        "screen_msgs.cpp",
        "terminal_msgs.cpp",
    ],
    hdrs = [
        "cmdframe_msgs.hpp",
        "codec.hpp",
        "msgs.hpp",
        "null_message.hpp",
        "screen_msgs.hpp",
        "status.hpp",
        "terminal_msgs.hpp",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//utils/ipc",
        "//utils/logger",
        "//utils/utf8",
    ],
)

exports_files(["README.md"])

cc_test(
    name = "msgs_test",
    size = "small",
    srcs = [
        "cmdframe_msgs_test.cpp",
        "codec_test.cpp",
        "screen_msgs_test.cpp",
        "terminal_msgs_test.cpp",
    ],
    deps = [
        ":msgs",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
