load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

cc_library(
    name = "ipc",
    srcs = [
        "channel.cpp",
        "directory.cpp",
        "filebuffer_channel.cpp",
        "framed_io.cpp",
        "framed_io.hpp",
        "metachannel.cpp",
        "msg.cpp",
        "smem_channel.cpp",
        "socket_channel.cpp",
    ],
    hdrs = [
        "channel.hpp",
        "directory.hpp",
        "filebuffer_channel.hpp",
        "ipc.hpp",
        "metachannel.hpp",
        "msg.hpp",
        "smem_channel.hpp",
        "socket_channel.hpp",
        "status.hpp",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//utils/logger",
        "//utils/multithreading",
        "//utils/timer",
    ],
)

exports_files(["README.md"])

cc_test(
    name = "ipc_test",
    size = "small",
    srcs = [
        "channel_test.cpp",
        "directory_test.cpp",
        "filebuffer_channel_test.cpp",
        "framed_io_test.cpp",
        "metachannel_test.cpp",
        "msg_test.cpp",
        "socket_channel_test.cpp",
    ],
    deps = [
        ":ipc",
        "//utils/multithreading",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
