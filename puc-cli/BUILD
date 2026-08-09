load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

cc_binary(
    name = "puc",
    srcs = ["main.cc"],
    deps = [
        "//utils/logger",
    ],
)
