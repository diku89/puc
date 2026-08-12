load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

cc_library(
    name = "commands",
    srcs = [
        "command.cpp",
        "config.cpp",
        "quit.cpp",
    ],
    hdrs = [
        "command.hpp",
        "config.hpp",
        "quit.hpp",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//msgs",
        "//properties",
        "//state",
        "//utils/containers:trie",
        "//utils/ipc",
        "//utils/logger",
    ],
)

cc_test(
    name = "command_test",
    size = "small",
    srcs = [
        "command_test.cpp",
        "config_test.cpp",
    ],
    deps = [
        ":commands",
        "//msgs",
        "//properties",
        "//state",
        "//utils/ipc",
        "//utils/multithreading",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
