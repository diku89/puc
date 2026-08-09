load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")
load("//utils/config:puc_config.bzl", "puc_config")

code_docs_sources(extra_srcs = ["terminal_test.cpp"])

config_setting(
    name = "linux",
    constraint_values = ["@platforms//os:linux"],
)

puc_config(
    name = "input_config",
    src = "universal_defaults.toml",
    install_path = "input_keys.toml",
    visibility = ["//visibility:public"],
)

puc_config(
    name = "darwin_defaults_config",
    src = "darwin-defaults.toml",
    install_path = "darwin-defaults.toml",
    visibility = ["//visibility:public"],
)

puc_config(
    name = "linux_defaults_config",
    src = "linux-defaults.toml",
    install_path = "linux-defaults.toml",
    visibility = ["//visibility:public"],
)

puc_config(
    name = "bsd_defaults_config",
    src = "bsd-defaults.toml",
    install_path = "bsd-defaults.toml",
    visibility = ["//visibility:public"],
)

puc_config(
    name = "timeout_config",
    src = "terminal_timeouts.toml",
    install_path = "terminal_timeouts.toml",
    visibility = ["//visibility:public"],
)

cc_library(
    name = "terminal",
    srcs = [
        "clipboard.cpp",
        "decoder.cpp",
        "input.cpp",
        "sequences.cpp",
        "session.cpp",
        "timeouts.cpp",
    ],
    hdrs = [
        "clipboard.hpp",
        "decoder.hpp",
        "event.hpp",
        "input.hpp",
        "sequences.hpp",
        "session.hpp",
        "status.hpp",
        "timeouts.hpp",
    ],
    linkopts = ["-lncurses"],
    visibility = ["//visibility:public"],
    deps = [
        "//msgs",
        "//utils/config",
        "//utils/containers:trie",
        "//utils/ipc",
        "//utils/logger",
    ],
)

cc_binary(
    name = "terminal-test",
    srcs = [
        "terminal_test.cpp",
        "terminal_test_options.cpp",
        "terminal_test_options.hpp",
        "terminal_test_runner.cpp",
        "terminal_test_runner.hpp",
        "terminal_test_selection.cpp",
        "terminal_test_selection.hpp",
    ],
    data = [
        ":bsd_defaults_config",
        ":darwin_defaults_config",
        ":input_config",
        ":linux_defaults_config",
        ":timeout_config",
    ],
    deps = [
        ":terminal",
        "//msgs",
        "//puc-cli/tui",
        "//utils/config",
        "//utils/ipc",
        "//utils/logger",
        "//utils/metronome",
        "//utils/multithreading",
    ],
)

cc_test(
    name = "terminal_test",
    size = "small",
    srcs = [
        "clipboard_test.cpp",
        "decoder_test.cpp",
        "event_test.cpp",
        "input_test.cpp",
        "sequences_test.cpp",
        "session_test.cpp",
        "status_test.cpp",
        "terminal_test_options.cpp",
        "terminal_test_options.hpp",
        "terminal_test_options_test.cpp",
        "terminal_test_runner.cpp",
        "terminal_test_runner.hpp",
        "terminal_test_runner_test.cpp",
        "terminal_test_selection.cpp",
        "terminal_test_selection.hpp",
        "terminal_test_selection_test.cpp",
        "timeouts_test.cpp",
    ],
    data = [
        ":bsd_defaults_config",
        ":darwin_defaults_config",
        ":input_config",
        ":linux_defaults_config",
        ":timeout_config",
    ],
    linkopts = select({
        ":linux": ["-lutil"],
        "//conditions:default": [],
    }),
    deps = [
        ":terminal",
        "//puc-cli/tui",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
