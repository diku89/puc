load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

cc_library(
    name = "presentation_tree",
    srcs = ["presentation_tree.cpp"],
    hdrs = ["presentation_tree.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//canvas/protos:canvas_cc_proto",
        "//canvas/protos/datastore:presentation_datastore",
        "//canvas/turn:address",
        "//utils/hash",
    ],
)
