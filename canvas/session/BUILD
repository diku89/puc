load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

cc_library(
    name = "turn_pipeline",
    srcs = ["turn_pipeline.cpp"],
    hdrs = ["turn_pipeline.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//canvas/protos:canvas_cc_proto",
        "//canvas/protos/datastore:database",
        "//utils/execution_graph",
        "//utils/multithreading",
    ],
)

cc_library(
    name = "orchestration_subsystem",
    srcs = ["orchestration_subsystem.cpp"],
    hdrs = ["orchestration_subsystem.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":turn_pipeline",
        "//canvas:canvas_subsystem",
        "//canvas:datastore_subsystem",
        "//canvas/presentation:presentation_tree",
        "//canvas/protos:canvas_cc_proto",
        "//canvas/protos/datastore:database",
        "//canvas/protos/datastore:presentation_datastore",
        "//state",
        "//utils/hash",
        "//utils/logger",
    ],
)

cc_test(
    name = "turn_pipeline_test",
    size = "small",
    srcs = ["turn_pipeline_test.cpp"],
    deps = [
        ":turn_pipeline",
        "//canvas/presentation:presentation_tree",
        "//canvas/protos:canvas_cc_proto",
        "//canvas/protos/datastore:canvas_datastore",
        "//canvas/protos/datastore:database",
        "//canvas/protos/datastore:presentation_datastore",
        "//canvas/protos/datastore:turn_datastore",
        "//canvas/turn:turn_tree",
        "//utils/multithreading",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
