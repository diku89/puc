"""Helpers for exposing repository-owned documented sources to Doxygen."""

_SOURCE_PATTERNS = [
    "**/*.c",
    "**/*.cc",
    "**/*.cpp",
    "**/*.cxx",
    "**/*.h",
    "**/*.hh",
    "**/*.hpp",
    "**/*.hxx",
    "**/*.py",
    "**/*.sh",
    # Git requires extensionless hook names, so include their known families
    # explicitly instead of broadening Doxygen to every extensionless file.
    "hooks/post-*",
    "hooks/pre-*",
    "hooks/sync-submodules",
]

def code_docs_sources(name = "code_docs_srcs", exclude = [], extra_srcs = []):
    """Exports non-test sources plus explicitly named manual applications."""
    native.filegroup(
        name = name,
        srcs = native.glob(
            _SOURCE_PATTERNS,
            allow_empty = True,
            # Root-package globs can follow Bazel's workspace output symlinks;
            # excluding them avoids documenting generated toolchains recursively.
            exclude = [
                "**/*_test.*",
                "bazel-*/**",
                "third_party/**",
            ] + exclude,
        ) + extra_srcs,
        visibility = ["//visibility:public"],
    )
