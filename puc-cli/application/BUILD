load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

config_setting(
    name = "linux",
    constraint_values = ["@platforms//os:linux"],
)

cc_library(
    name = "control",
    srcs = ["application_control_subsystem.cpp"],
    hdrs = [
        "application_control.hpp",
        "application_control_subsystem.hpp",
    ],
    visibility = ["//visibility:public"],
    deps = ["//state"],
)

cc_library(
    name = "bootstrap",
    srcs = ["bootstrap.cpp"],
    hdrs = ["bootstrap.hpp"],
    data = [
        "//canvas:canvas_config",
        "//utils/ipc:ipc_config",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":control",
        "//canvas:canvas_subsystem",
        "//canvas:datastore_subsystem",
        "//canvas/session:orchestration_subsystem",
        "//commands:command_subsystems",
        "//properties:properties_subsystem",
        "//puc-cli/tui:input_subsystem",
        "//puc-cli/tui:rendering_subsystems",
        "//puc-cli/tui/terminal:terminal_subsystems",
        "//state",
        "//themes:theme_subsystem",
        "//utils/ipc:ipc_subsystems",
        "//utils/logger:logger_subsystem",
        "//utils/metronome:metronome_subsystem",
        "//utils/multithreading:worker_subsystem",
        "//utils/timer:timer_subsystem",
    ],
)

cc_test(
    name = "subsystems_test",
    size = "small",
    srcs = ["subsystems_test.cpp"],
    data = ["//themes:default_dark_config"],
    linkopts = select({
        ":linux": ["-lutil"],
        "//conditions:default": [],
    }),
    deps = [
        ":bootstrap",
        ":control",
        "//commands",
        "//commands:command_subsystems",
        "//msgs",
        "//properties",
        "//properties:properties_subsystem",
        "//puc-cli/tui",
        "//puc-cli/tui:input_subsystem",
        "//puc-cli/tui:rendering_subsystems",
        "//puc-cli/tui/terminal",
        "//puc-cli/tui/terminal:terminal_subsystems",
        "//state",
        "//themes:theme_subsystem",
        "//utils/ipc",
        "//utils/ipc:ipc_subsystems",
        "//utils/logger",
        "//utils/logger:logger_subsystem",
        "//utils/metronome:metronome_subsystem",
        "//utils/multithreading",
        "//utils/multithreading:worker_subsystem",
        "//utils/timer:timer_subsystem",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
