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
        "channels.cpp",
        "command_mode.cpp",
        "commands.cpp",
        "configuration.cpp",
        "directory.cpp",
        "embedded_terminal.cpp",
        "input.cpp",
        "logger.cpp",
        "metronome.cpp",
        "presentation.cpp",
        "screen.cpp",
        "terminal.cpp",
        "workers.cpp",
    ],
    hdrs = [
        "adapters.hpp",
        "bootstrap.hpp",
        "channels.hpp",
        "command_mode.hpp",
        "commands.hpp",
        "configuration.hpp",
        "directory.hpp",
        "embedded_terminal.hpp",
        "input.hpp",
        "logger.hpp",
        "metronome.hpp",
        "presentation.hpp",
        "screen.hpp",
        "terminal.hpp",
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
        "//puc-cli/terminal",
        "//puc-cli/tui",
        "//utils/config",
        "//utils/ipc",
        "//utils/logger",
        "//utils/metronome",
        "//utils/multithreading",
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
