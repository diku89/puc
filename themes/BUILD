load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")
load("//utils/config:puc_config.bzl", "puc_config")

code_docs_sources()

puc_config(
    name = "default_dark_config",
    src = "default-dark.toml",
    install_path = "themes/default-dark.toml",
    visibility = ["//visibility:public"],
)

cc_library(
    name = "themes",
    srcs = ["themes.cpp"],
    hdrs = ["themes.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//properties",
        "//puc-cli/tui",
    ],
)

cc_library(
    name = "theme_subsystem",
    srcs = ["theme_subsystem.cpp"],
    hdrs = ["theme_subsystem.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":themes",
        "//properties:properties_subsystem",
        "//state",
    ],
)

cc_test(
    name = "themes_test",
    size = "small",
    srcs = ["themes_test.cpp"],
    data = [":default_dark_config"],
    deps = [
        ":themes",
        "//properties",
        "//puc-cli/tui",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
