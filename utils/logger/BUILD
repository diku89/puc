load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

cc_library(
    name = "logger",
    srcs = ["logger.cc"],
    hdrs = ["logger.hpp"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "logger_subsystem",
    srcs = ["logger_subsystem.cpp"],
    hdrs = ["logger_subsystem.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":logger",
        "//state",
    ],
)

cc_test(
    name = "logger_test",
    size = "small",
    srcs = ["logger_test.cc"],
    deps = [":logger"],
)
