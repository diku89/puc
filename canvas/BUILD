load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")
load("//utils/config:puc_config.bzl", "puc_config")

code_docs_sources()

puc_config(
    name = "canvas_config",
    src = "canvas.toml",
    install_path = "canvas.toml",
    visibility = ["//visibility:public"],
)

cc_library(
    name = "canvas_id",
    srcs = ["canvas_id.cpp"],
    hdrs = ["canvas_id.hpp"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "settings",
    srcs = ["settings.cpp"],
    hdrs = ["settings.hpp"],
    visibility = ["//visibility:public"],
    deps = ["//properties"],
)

cc_library(
    name = "datastore_subsystem",
    srcs = ["datastore_subsystem.cpp"],
    hdrs = ["datastore_subsystem.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":settings",
        "//canvas/protos/datastore:canvas_datastore",
        "//canvas/protos/datastore:database",
        "//canvas/protos/datastore:presentation_datastore",
        "//canvas/protos/datastore:turn_datastore",
        "//properties:properties_subsystem",
        "//state",
        "//utils/logger",
        "//utils/logger:logger_subsystem",
    ],
)

cc_library(
    name = "canvas_subsystem",
    srcs = ["canvas_subsystem.cpp"],
    hdrs = ["canvas_subsystem.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":canvas_id",
        ":datastore_subsystem",
        "//canvas/protos:canvas_cc_proto",
        "//canvas/protos/datastore:canvas_datastore",
        "//canvas/protos/datastore:database",
        "//canvas/protos/datastore:turn_datastore",
        "//canvas/session:turn_pipeline",
        "//canvas/turn:turn_tree",
        "//state",
        "//utils/ipc",
        "//utils/ipc:ipc_subsystems",
        "//utils/logger",
        "//utils/multithreading:worker_subsystem",
    ],
)

cc_test(
    name = "canvas_subsystem_test",
    size = "small",
    srcs = ["canvas_subsystem_test.cpp"],
    deps = [
        ":canvas_subsystem",
        ":datastore_subsystem",
        "//canvas/presentation:presentation_tree",
        "//canvas/protos:canvas_cc_proto",
        "//canvas/protos/datastore:canvas_datastore",
        "//canvas/protos/datastore:database",
        "//canvas/protos/datastore:presentation_datastore",
        "//canvas/protos/datastore:turn_datastore",
        "//canvas/session:orchestration_subsystem",
        "//canvas/session:turn_pipeline",
        "//canvas/turn:turn_tree",
        "//properties:properties_subsystem",
        "//state",
        "//utils/ipc",
        "//utils/ipc:ipc_subsystems",
        "//utils/logger:logger_subsystem",
        "//utils/multithreading:worker_subsystem",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
