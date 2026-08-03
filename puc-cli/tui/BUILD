load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

config_setting(
    name = "linux",
    constraint_values = ["@platforms//os:linux"],
)

cc_library(
    name = "tui",
    srcs = [
        "canvas.cpp",
        "layout.cpp",
        "screen.cpp",
        "theme.cpp",
        "zbuf.cpp",
    ],
    hdrs = [
        "canvas.hpp",
        "frame.hpp",
        "layout.hpp",
        "screen.hpp",
        "state.hpp",
        "status.hpp",
        "theme.hpp",
        "zbuf.hpp",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//utils/logger",
    ],
)

cc_binary(
    name = "test-app",
    srcs = ["test-app.cpp"],
    deps = [
        ":tui",
        "//utils/logger",
    ],
)

cc_test(
    name = "tui_test",
    size = "small",
    srcs = [
        "canvas_test.cpp",
        "layout_test.cpp",
        "screen_test.cpp",
        "status_test.cpp",
        "theme_test.cpp",
        "zbuf_test.cpp",
    ],
    linkopts = select({
        ":linux": ["-lutil"],
        "//conditions:default": [],
    }),
    deps = [
        ":tui",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
