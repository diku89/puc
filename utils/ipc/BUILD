load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//docs:code_sources.bzl", "code_docs_sources")
load("//utils/config:puc_config.bzl", "puc_config")

code_docs_sources()

puc_config(
    name = "ipc_config",
    src = "ipc.toml",
    install_path = "ipc.toml",
    visibility = ["//visibility:public"],
)

cc_library(
    name = "settings",
    srcs = ["settings.cpp"],
    hdrs = ["settings.hpp"],
    visibility = ["//visibility:public"],
    deps = ["//properties"],
)

cc_library(
    name = "ipc",
    srcs = [
        "channel.cpp",
        "channel_path.cpp",
        "directory.cpp",
        "filebuffer_channel.cpp",
        "framed_io.cpp",
        "framed_io.hpp",
        "metachannel.cpp",
        "msg.cpp",
        "smem_channel.cpp",
        "socket_channel.cpp",
    ],
    hdrs = [
        "channel.hpp",
        "channel_path.hpp",
        "directory.hpp",
        "filebuffer_channel.hpp",
        "ipc.hpp",
        "metachannel.hpp",
        "msg.hpp",
        "smem_channel.hpp",
        "socket_channel.hpp",
        "status.hpp",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//utils/hash",
        "//utils/logger",
        "//utils/multithreading",
        "//utils/timer",
    ],
)

cc_library(
    name = "ipc_subsystems",
    srcs = [
        "channel_subsystems.cpp",
        "directory_subsystem.cpp",
    ],
    hdrs = [
        "channel_subsystems.hpp",
        "directory_subsystem.hpp",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":ipc",
        ":settings",
        "//msgs",
        "//properties:properties_subsystem",
        "//state",
        "//utils/multithreading",
        "//utils/multithreading:worker_subsystem",
    ],
)

cc_test(
    name = "settings_test",
    size = "small",
    srcs = ["settings_test.cpp"],
    deps = [
        ":settings",
        "//properties",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

exports_files(["README.md"])

cc_test(
    name = "ipc_test",
    size = "small",
    srcs = [
        "channel_test.cpp",
        "directory_test.cpp",
        "filebuffer_channel_test.cpp",
        "framed_io_test.cpp",
        "metachannel_test.cpp",
        "msg_test.cpp",
        "socket_channel_test.cpp",
    ],
    deps = [
        ":ipc",
        "//utils/multithreading",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
