load("@rules_python//python:py_binary.bzl", "py_binary")
load("@rules_python//python:py_test.bzl", "py_test")
load("@rules_shell//shell:sh_binary.bzl", "sh_binary")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

exports_files([
    "format.sh",
    "update_docs.sh",
])

sh_binary(
    name = "valgrind",
    srcs = ["valgrind.sh"],
)

py_binary(
    name = "doxygen_xml_to_markdown",
    srcs = ["doxygen_xml_to_markdown.py"],
    legacy_create_init = 0,
    python_version = "3.12",
)

py_test(
    name = "doxygen_xml_to_markdown_test",
    size = "small",
    srcs = [
        "doxygen_xml_to_markdown.py",
        "doxygen_xml_to_markdown_test.py",
    ],
    legacy_create_init = 0,
    main = "doxygen_xml_to_markdown_test.py",
    python_version = "3.12",
)
