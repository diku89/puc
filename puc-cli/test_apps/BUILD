load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources(extra_srcs = ["terminal/terminal_test.cpp"])

config_setting(
    name = "linux",
    constraint_values = ["@platforms//os:linux"],
)

cc_binary(
    name = "test-app",
    srcs = [
        "tui/test-app.cpp",
        "tui/tui_test_runtime.cpp",
        "tui/tui_test_runtime.hpp",
    ],
    data = ["//themes:default_dark_config"],
    deps = [
        "//puc-cli/application:bootstrap",
        "//puc-cli/tui",
        "//puc-cli/tui:rendering_subsystems",
        "//puc-cli/tui/terminal",
        "//state",
        "//themes:theme_subsystem",
        "//utils/logger",
        "//utils/timer",
    ],
)

cc_binary(
    name = "input-test-app",
    srcs = [
        "input/input-test-app.cpp",
        "input/input_test_runtime.cpp",
        "input/input_test_runtime.hpp",
    ],
    data = [
        "//puc-cli/tui/terminal:bsd_defaults_config",
        "//puc-cli/tui/terminal:darwin_defaults_config",
        "//puc-cli/tui/terminal:input_config",
        "//puc-cli/tui/terminal:linux_defaults_config",
        "//puc-cli/tui/terminal:timeout_config",
        "//puc-cli/tui/terminal:xterm_ghostty_defaults_config",
        "//themes:default_dark_config",
    ],
    linkopts = select({
        ":linux": ["-lutil"],
        "//conditions:default": [],
    }),
    deps = [
        "//commands:command_subsystems",
        "//msgs",
        "//puc-cli/application:bootstrap",
        "//puc-cli/application:control",
        "//puc-cli/tui",
        "//puc-cli/tui:input_subsystem",
        "//puc-cli/tui:rendering_subsystems",
        "//puc-cli/tui/terminal",
        "//puc-cli/tui/terminal:terminal_subsystems",
        "//state",
        "//themes:theme_subsystem",
        "//utils/ipc",
        "//utils/logger",
        "//utils/timer",
    ],
)

cc_library(
    name = "terminal_test_support",
    srcs = [
        "terminal/terminal_test_options.cpp",
        "terminal/terminal_test_runner.cpp",
        "terminal/terminal_test_selection.cpp",
    ],
    hdrs = [
        "terminal/terminal_test_options.hpp",
        "terminal/terminal_test_runner.hpp",
        "terminal/terminal_test_selection.hpp",
    ],
    deps = [
        "//puc-cli/tui",
        "//puc-cli/tui/terminal",
    ],
)

cc_binary(
    name = "terminal-test",
    srcs = [
        "terminal/terminal_test.cpp",
        "terminal/terminal_test_runtime.cpp",
        "terminal/terminal_test_runtime.hpp",
    ],
    data = [
        "//puc-cli/tui/terminal:bsd_defaults_config",
        "//puc-cli/tui/terminal:darwin_defaults_config",
        "//puc-cli/tui/terminal:input_config",
        "//puc-cli/tui/terminal:linux_defaults_config",
        "//puc-cli/tui/terminal:timeout_config",
        "//puc-cli/tui/terminal:xterm_ghostty_defaults_config",
        "//themes:default_dark_config",
    ],
    deps = [
        ":terminal_test_support",
        "//msgs",
        "//properties",
        "//puc-cli/application:bootstrap",
        "//puc-cli/application:control",
        "//puc-cli/tui",
        "//puc-cli/tui:rendering_subsystems",
        "//puc-cli/tui/terminal",
        "//puc-cli/tui/terminal:terminal_subsystems",
        "//state",
        "//themes:theme_subsystem",
        "//utils/ipc",
        "//utils/logger",
        "//utils/metronome",
        "//utils/metronome:metronome_subsystem",
        "//utils/timer",
    ],
)

cc_test(
    name = "terminal_test_support_test",
    size = "small",
    srcs = [
        "terminal/terminal_test_options_test.cpp",
        "terminal/terminal_test_runner_test.cpp",
        "terminal/terminal_test_selection_test.cpp",
    ],
    deps = [
        ":terminal_test_support",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
