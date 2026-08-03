load("@rules_cc//cc:cc_library.bzl", "cc_library")

exports_files(["googletest_1_17_0_char8_t.patch"])

cc_library(
    name = "tomlc17",
    srcs = ["tomlc17/src/tomlc17.c"],
    hdrs = ["tomlc17/src/tomlc17.h"],
    copts = ["-std=c17"],
    strip_include_prefix = "tomlc17/src",
    visibility = ["//visibility:public"],
)
