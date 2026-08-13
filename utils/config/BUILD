load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

exports_files(["puc_config.bzl"])

cc_library(
    name = "config",
    srcs = ["config.cpp"],
    hdrs = ["config.hpp"],
    visibility = ["//properties:__pkg__"],
    deps = [
        "//third_party:tomlc17",
        "//utils/logger",
    ],
)

cc_test(
    name = "config_test",
    size = "small",
    srcs = ["config_test.cpp"],
    deps = [
        ":config",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
