load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

config_setting(
    name = "linux",
    constraint_values = ["@platforms//os:linux"],
)

cc_library(
    name = "state",
    srcs = ["state.cpp"],
    hdrs = [
        "application_control.hpp",
        "lifecycle.hpp",
        "state.hpp",
    ],
    visibility = ["//visibility:public"],
    deps = ["//utils/execution_graph:dependency_graph"],
)

cc_library(
    name = "adapters",
    srcs = [
        "bootstrap.cpp",
        "builtin_commands.cpp",
        "channels.cpp",
        "command_mode.cpp",
        "commands.cpp",
        "control.cpp",
        "directory.cpp",
        "embedded_terminal.cpp",
        "input.cpp",
        "logger.cpp",
        "metronome.cpp",
        "presentation.cpp",
        "properties.cpp",
        "screen.cpp",
        "terminal.cpp",
        "theme.cpp",
        "timer.cpp",
        "workers.cpp",
    ],
    hdrs = [
        "adapters.hpp",
        "bootstrap.hpp",
        "builtin_commands.hpp",
        "channels.hpp",
        "command_mode.hpp",
        "commands.hpp",
        "control.hpp",
        "directory.hpp",
        "embedded_terminal.hpp",
        "input.hpp",
        "logger.hpp",
        "metronome.hpp",
        "presentation.hpp",
        "properties.hpp",
        "screen.hpp",
        "terminal.hpp",
        "theme.hpp",
        "timer.hpp",
        "workers.hpp",
    ],
    linkopts = select({
        ":linux": ["-lutil"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
    deps = [
        ":state",
        "//commands",
        "//msgs",
        "//properties",
        "//puc-cli/terminal",
        "//puc-cli/tui",
        "//themes",
        "//utils/ipc",
        "//utils/logger",
        "//utils/metronome",
        "//utils/multithreading",
        "//utils/timer",
    ],
)

cc_test(
    name = "state_test",
    size = "small",
    srcs = ["state_test.cpp"],
    deps = [
        ":state",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "adapters_test",
    size = "small",
    srcs = ["adapters_test.cpp"],
    data = ["//themes:default_dark_config"],
    deps = [
        ":adapters",
        ":state",
        "//utils/ipc",
        "//utils/logger",
        "//utils/multithreading",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
