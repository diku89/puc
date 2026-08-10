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
        "annotated_text_frame.cpp",
        "bounding_frame.cpp",
        "canvas.cpp",
        "cmd_frame.cpp",
        "frame.cpp",
        "input_frame.cpp",
        "integrated_term_frame.cpp",
        "layout.cpp",
        "renderer.cpp",
        "screen.cpp",
        "selection.cpp",
        "text_editor_utils.cpp",
        "text_input_frame.cpp",
        "theme.cpp",
        "zbuf.cpp",
    ],
    hdrs = [
        "annotated_text_frame.hpp",
        "bounding_frame.hpp",
        "canvas.hpp",
        "cmd_frame.hpp",
        "frame.hpp",
        "input_frame.hpp",
        "integrated_term_frame.hpp",
        "layout.hpp",
        "renderer.hpp",
        "screen.hpp",
        "selection.hpp",
        "status.hpp",
        "text_editor_utils.hpp",
        "text_input_frame.hpp",
        "theme.hpp",
        "zbuf.hpp",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//msgs",
        "//puc-cli/terminal",
        "//third_party:tmt",
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

cc_binary(
    name = "input-test-app",
    srcs = ["input-test-app.cpp"],
    data = [
        "//puc-cli/terminal:bsd_defaults_config",
        "//puc-cli/terminal:darwin_defaults_config",
        "//puc-cli/terminal:input_config",
        "//puc-cli/terminal:linux_defaults_config",
        "//puc-cli/terminal:timeout_config",
        "//puc-cli/terminal:xterm_ghostty_defaults_config",
    ],
    linkopts = select({
        ":linux": ["-lutil"],
        "//conditions:default": [],
    }),
    deps = [
        ":tui",
        "//msgs",
        "//puc-cli/state:adapters",
        "//puc-cli/terminal",
        "//utils/config",
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
        "//puc-cli/terminal:xterm_ghostty_defaults_config",
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

cc_test(
    name = "annotated_text_frame_test",
    size = "small",
    srcs = ["annotated_text_frame_test.cpp"],
    deps = [
        ":tui",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "bounding_frame_test",
    size = "small",
    srcs = ["bounding_frame_test.cpp"],
    deps = [
        ":tui",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "cmd_frame_test",
    size = "small",
    srcs = ["cmd_frame_test.cpp"],
    deps = [
        ":tui",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "input_frame_test",
    size = "small",
    srcs = ["input_frame_test.cpp"],
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

cc_test(
    name = "integrated_term_frame_test",
    size = "small",
    srcs = ["integrated_term_frame_test.cpp"],
    deps = [
        ":tui",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "text_editor_utils_test",
    size = "small",
    srcs = ["text_editor_utils_test.cpp"],
    deps = [
        ":tui",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "text_input_frame_test",
    size = "small",
    srcs = ["text_input_frame_test.cpp"],
    deps = [
        ":tui",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
