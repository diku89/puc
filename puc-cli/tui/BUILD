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
        "frame.cpp",
        "layout.cpp",
        "renderer.cpp",
        "screen.cpp",
        "selection.cpp",
        "theme.cpp",
        "zbuf.cpp",
    ],
    hdrs = [
        "canvas.hpp",
        "frame.hpp",
        "layout.hpp",
        "renderer.hpp",
        "screen.hpp",
        "selection.hpp",
        "status.hpp",
        "theme.hpp",
        "zbuf.hpp",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//msgs",
        "//puc-cli/terminal",
        "//utils/execution_graph",
        "//utils/ipc",
        "//utils/logger",
        "//utils/multithreading",
    ],
)

cc_binary(
    name = "test-app",
    srcs = ["test-app.cpp"],
    deps = [
        ":tui",
        "//utils/logger",
        "//utils/multithreading",
    ],
)

cc_test(
    name = "tui_test",
    size = "small",
    srcs = [
        "canvas_test.cpp",
        "layout_test.cpp",
        "renderer_test.cpp",
        "screen_test.cpp",
        "selection_test.cpp",
        "status_test.cpp",
        "theme_test.cpp",
        "zbuf_test.cpp",
    ],
    data = [
        "//puc-cli/terminal:bsd_defaults_config",
        "//puc-cli/terminal:darwin_defaults_config",
        "//puc-cli/terminal:input_config",
        "//puc-cli/terminal:linux_defaults_config",
    ],
    linkopts = select({
        ":linux": ["-lutil"],
        "//conditions:default": [],
    }),
    deps = [
        ":tui",
        "//utils/multithreading",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
