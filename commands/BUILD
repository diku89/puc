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
        "//puc-cli/application:control",
        "//utils/containers:trie",
        "//utils/ipc",
        "//utils/logger",
    ],
)

cc_library(
    name = "command_subsystems",
    srcs = [
        "builtin_command_subsystem.cpp",
        "command_mode_subsystem.cpp",
        "command_subsystem.cpp",
    ],
    hdrs = [
        "builtin_command_subsystem.hpp",
        "command_mode_subsystem.hpp",
        "command_subsystem.hpp",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":commands",
        "//properties:properties_subsystem",
        "//puc-cli/application:control",
        "//puc-cli/tui",
        "//puc-cli/tui:input_subsystem",
        "//puc-cli/tui:rendering_subsystems",
        "//puc-cli/tui/terminal",
        "//state",
        "//utils/ipc:ipc_subsystems",
        "//utils/multithreading:worker_subsystem",
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
        "//puc-cli/application:control",
        "//utils/ipc",
        "//utils/multithreading",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
