load("@doxygen//:doxygen.bzl", "doxygen")

# Bazel globs cannot cross package boundaries. Each repository-owned source
# package exports a glob-backed code_docs_srcs filegroup with code_docs_sources().
doxygen(
    name = "code_xml",
    srcs = [
        "//:code_docs_srcs",
        "//github/ci_tools:code_docs_srcs",
        "//puc-cli:code_docs_srcs",
        "//utils/logger:code_docs_srcs",
        "//utils/scripts:code_docs_srcs",
    ],
    outs = ["xml"],
    # Shell is mapped to Python so its @file guide is recognized. Each script
    # hides its executable body from the symbol index because shell syntax is
    # not a Python API.
    extension_mapping = [
        "no_extension=Python",
        "sh=Python",
    ],
    file_patterns = [
        "*.c",
        "*.cc",
        "*.cpp",
        "*.cxx",
        "*.h",
        "*.hh",
        "*.hpp",
        "*.hxx",
        "*.py",
        "*.sh",
        "post-*",
        "pre-*",
        "sync-submodules",
    ],
    generate_html = False,
    generate_latex = False,
    generate_xml = True,
    project_name = "puc",
    # Interpret Python docstrings as Doxygen markup so @param, @return, and
    # related commands become structured reference sections.
    python_docstring = False,
    # Documentation is generated explicitly on supported Linux and macOS hosts.
    tags = ["manual"],
    visibility = ["//visibility:public"],
    xml_output = "xml",
    xml_programlisting = False,
)
