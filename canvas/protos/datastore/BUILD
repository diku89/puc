load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

cc_library(
    name = "database",
    srcs = ["database.cpp"],
    hdrs = [
        "database.hpp",
        "status.hpp",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//utils/hash",
        "@sqlite3",
    ],
)

cc_library(
    name = "canvas_datastore",
    srcs = ["canvas_datastore.cpp"],
    hdrs = ["canvas_datastore.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":database",
        "//canvas/protos:canvas_cc_proto",
        "//utils/hash",
    ],
)

cc_library(
    name = "turn_datastore",
    srcs = ["turn_datastore.cpp"],
    hdrs = ["turn_datastore.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":database",
        "//canvas/protos:canvas_cc_proto",
        "//canvas/turn:address",
        "//utils/hash",
    ],
)

cc_library(
    name = "presentation_datastore",
    srcs = ["presentation_datastore.cpp"],
    hdrs = ["presentation_datastore.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":database",
        "//canvas/protos:canvas_cc_proto",
        "//utils/hash",
    ],
)

cc_test(
    name = "database_test",
    size = "small",
    srcs = ["database_test.cpp"],
    deps = [
        ":database",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
