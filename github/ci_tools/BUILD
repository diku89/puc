load("@rules_python//python:py_test.bzl", "py_test")
load("//docs:code_sources.bzl", "code_docs_sources")

code_docs_sources()

py_test(
    name = "check_authors_test",
    size = "small",
    srcs = [
        "check_authors.py",
        "check_authors_test.py",
    ],
    legacy_create_init = 0,
    main = "check_authors_test.py",
    python_version = "3.12",
)

py_test(
    name = "check_signatures_test",
    size = "small",
    srcs = [
        "check_signatures.py",
        "check_signatures_test.py",
    ],
    legacy_create_init = 0,
    main = "check_signatures_test.py",
    python_version = "3.12",
)
