load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

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
        "commands.cpp",
        "directory.cpp",
        "input.cpp",
        "logger.cpp",
        "screen.cpp",
        "terminal.cpp",
        "workers.cpp",
    ],
    hdrs = [
        "adapters.hpp",
        "bootstrap.hpp",
        "channels.hpp",
        "commands.hpp",
        "directory.hpp",
        "input.hpp",
        "logger.hpp",
        "screen.hpp",
        "terminal.hpp",
        "workers.hpp",
    ],
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
