load("@doxygen//:doxygen.bzl", "doxygen")

# Bazel globs cannot cross package boundaries. Each repository-owned source
# package exports a glob-backed code_docs_srcs filegroup with code_docs_sources().
doxygen(
    name = "code_xml",
    srcs = [
        "//:code_docs_srcs",
        "//github/ci_tools:code_docs_srcs",
        "//puc-cli:code_docs_srcs",
        "//puc-cli/tui:code_docs_srcs",
        "//utils/containers:code_docs_srcs",
        "//utils/logger:code_docs_srcs",
        "//utils/scripts:code_docs_srcs",
    ],
    outs = ["xml"],
    # Communication contracts share one cross-reference page. The double-pipe
    # separator keeps ordinary prose commas usable without escaping.
    aliases = [
        "channel{4||}=@xrefitem puc_protocol_contracts \\\"Protocol Contract\\\" \\\"PUC Protocol Contracts\\\" @b Kind: Channel @n @b Name: @c \\1 @n @b Purpose: \\2 @n @b Producers: \\3 @n @b Consumers: \\4",
        "msg{4||}=@xrefitem puc_protocol_contracts \\\"Protocol Contract\\\" \\\"PUC Protocol Contracts\\\" @b Kind: Message @n @b Type: @ref \\1 \\\"\\1\\\" @n @b Purpose: \\2 @n @b Producers: \\3 @n @b Consumers: \\4",
        "msg_id{5||}=@xrefitem puc_protocol_contracts \\\"Protocol Contract\\\" \\\"PUC Protocol Contracts\\\" @b Kind: Message ID @n @b ID: @c \\1 @n @b Message: @ref \\2 \\\"\\2\\\" @n @b Purpose: \\3 @n @b Producers: \\4 @n @b Consumers: \\5",
    ],
    # Doxygen's SYSTEM default follows filesystem case sensitivity, changing
    # XML refids and Markdown filenames between macOS and Linux.
    case_sense_names = False,
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
