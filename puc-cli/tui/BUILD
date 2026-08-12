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
        "frames/annotated_text_frame.cpp",
        "frames/bounding_frame.cpp",
        "frames/cmd_frame.cpp",
        "frames/input_frame.cpp",
        "frames/integrated_term_frame.cpp",
        "frames/message_frame.cpp",
        "frames/text_input_frame.cpp",
        "rendering/canvas.cpp",
        "rendering/frame.cpp",
        "rendering/layout.cpp",
        "rendering/renderer.cpp",
        "rendering/screen.cpp",
        "rendering/selection.cpp",
        "rendering/theme.cpp",
        "rendering/zbuf.cpp",
        "text_input/text_editor_utils.cpp",
    ],
    hdrs = [
        "frames/annotated_text_frame.hpp",
        "frames/bounding_frame.hpp",
        "frames/cmd_frame.hpp",
        "frames/input_frame.hpp",
        "frames/integrated_term_frame.hpp",
        "frames/message_frame.hpp",
        "frames/text_input_frame.hpp",
        "rendering/canvas.hpp",
        "rendering/frame.hpp",
        "rendering/layout.hpp",
        "rendering/renderer.hpp",
        "rendering/screen.hpp",
        "rendering/selection.hpp",
        "rendering/status.hpp",
        "rendering/theme.hpp",
        "rendering/zbuf.hpp",
        "text_input/text_editor_utils.hpp",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//msgs",
        "//puc-cli/tui/terminal",
        "//third_party:tmt",
        "//utils/execution_graph",
        "//utils/ipc",
        "//utils/logger",
        "//utils/multithreading",
    ],
)

cc_library(
    name = "input_subsystem",
    srcs = ["frames/input_subsystem.cpp"],
    hdrs = ["frames/input_subsystem.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":tui",
        "//msgs",
        "//state",
        "//utils/ipc",
        "//utils/ipc:ipc_subsystems",
    ],
)

cc_library(
    name = "rendering_subsystems",
    srcs = [
        "rendering/presentation_subsystem.cpp",
        "rendering/screen_subsystem.cpp",
    ],
    hdrs = [
        "rendering/presentation_subsystem.hpp",
        "rendering/screen_subsystem.hpp",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":tui",
        "//puc-cli/tui/terminal",
        "//puc-cli/tui/terminal:terminal_subsystems",
        "//state",
        "//utils/ipc",
        "//utils/ipc:ipc_subsystems",
        "//utils/multithreading",
        "//utils/multithreading:worker_subsystem",
    ],
)

cc_test(
    name = "tui_test",
    size = "small",
    srcs = [
        "rendering/canvas_test.cpp",
        "rendering/layout_test.cpp",
        "rendering/renderer_test.cpp",
        "rendering/screen_test.cpp",
        "rendering/selection_test.cpp",
        "rendering/status_test.cpp",
        "rendering/theme_test.cpp",
        "rendering/zbuf_test.cpp",
    ],
    data = [
        "//puc-cli/tui/terminal:bsd_defaults_config",
        "//puc-cli/tui/terminal:darwin_defaults_config",
        "//puc-cli/tui/terminal:input_config",
        "//puc-cli/tui/terminal:linux_defaults_config",
        "//puc-cli/tui/terminal:xterm_ghostty_defaults_config",
    ],
    linkopts = select({
        ":linux": ["-lutil"],
        "//conditions:default": [],
    }),
    deps = [
        ":tui",
        "//properties",
        "//utils/multithreading",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "annotated_text_frame_test",
    size = "small",
    srcs = ["frames/annotated_text_frame_test.cpp"],
    deps = [
        ":tui",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "bounding_frame_test",
    size = "small",
    srcs = ["frames/bounding_frame_test.cpp"],
    deps = [
        ":tui",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "cmd_frame_test",
    size = "small",
    srcs = ["frames/cmd_frame_test.cpp"],
    deps = [
        ":tui",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "input_frame_test",
    size = "small",
    srcs = ["frames/input_frame_test.cpp"],
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
    name = "message_frame_test",
    size = "small",
    srcs = ["frames/message_frame_test.cpp"],
    deps = [
        ":tui",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "integrated_term_frame_test",
    size = "small",
    srcs = ["frames/integrated_term_frame_test.cpp"],
    deps = [
        ":tui",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "text_editor_utils_test",
    size = "small",
    srcs = ["text_input/text_editor_utils_test.cpp"],
    deps = [
        ":tui",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "text_input_frame_test",
    size = "small",
    srcs = ["frames/text_input_frame_test.cpp"],
    deps = [
        ":tui",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
