load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")
load("//utils/config:puc_config.bzl", "puc_config")

code_docs_sources()

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
    name = "xterm_ghostty_defaults_config",
    src = "terminals/xterm-ghostty.toml",
    install_path = "terminals/xterm-ghostty.toml",
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
        "configuration_paths.cpp",
        "decoder.cpp",
        "event_messages.cpp",
        "input.cpp",
        "sequences.cpp",
        "session.cpp",
        "timeouts.cpp",
    ],
    hdrs = [
        "clipboard.hpp",
        "configuration_paths.hpp",
        "decoder.hpp",
        "event.hpp",
        "event_messages.hpp",
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
        "//properties",
        "//utils/containers:trie",
        "//utils/ipc",
        "//utils/logger",
    ],
)

cc_library(
    name = "terminal_subsystems",
    srcs = [
        "embedded_terminal_subsystem.cpp",
        "terminal_subsystem.cpp",
    ],
    hdrs = [
        "embedded_terminal_subsystem.hpp",
        "terminal_subsystem.hpp",
    ],
    linkopts = select({
        ":linux": ["-lutil"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
    deps = [
        ":terminal",
        "//msgs",
        "//properties",
        "//properties:properties_subsystem",
        "//puc-cli/tui",
        "//puc-cli/tui:input_subsystem",
        "//state",
        "//utils/ipc",
        "//utils/ipc:ipc_subsystems",
        "//utils/logger",
        "//utils/timer",
    ],
)

cc_test(
    name = "terminal_test",
    size = "small",
    srcs = [
        "clipboard_test.cpp",
        "decoder_test.cpp",
        "event_messages_test.cpp",
        "event_test.cpp",
        "input_test.cpp",
        "sequences_test.cpp",
        "session_test.cpp",
        "status_test.cpp",
        "timeouts_test.cpp",
    ],
    data = [
        ":bsd_defaults_config",
        ":darwin_defaults_config",
        ":input_config",
        ":linux_defaults_config",
        ":timeout_config",
        ":xterm_ghostty_defaults_config",
    ],
    linkopts = select({
        ":linux": ["-lutil"],
        "//conditions:default": [],
    }),
    deps = [
        ":terminal",
        "//properties",
        "//puc-cli/tui",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
