"""Declare TOML files installed below PUC's config root.

Tests consume a declaration through their `data` attribute. Bazel then exposes
the file at `install_path` relative to the test's runfiles workspace, which is
also the test process's initial working directory.

Example:

```starlark
puc_config(
    name = "input_config",
    src = "input_keys.toml",
    install_path = "terminal/input_keys.toml",
)

cc_test(
    name = "input_test",
    data = [":input_config"],
    # The test can open "terminal/input_keys.toml".
)
```
"""

PucConfigInfo = provider(
    doc = "One system-default TOML file and its config-root-relative destination.",
    fields = {
        "install_path": "Validated path relative to the installed config root.",
        "src": "The repository-owned TOML source File.",
    },
)

def _validate_install_path(path):
    """Reject paths that are not canonical config-root-relative TOML names."""
    if not path:
        fail("install_path must not be empty")
    if path.startswith("/") or path.endswith("/"):
        fail("install_path must be relative and name a file: %s" % path)
    if "\\" in path:
        fail("install_path must use '/' separators: %s" % path)
    for component in path.split("/"):
        if not component or component == "." or component == "..":
            fail("install_path must be a canonical relative path: %s" % path)
    if not path.endswith(".toml"):
        fail("install_path must name a .toml file: %s" % path)

def _puc_config_impl(ctx):
    _validate_install_path(ctx.attr.install_path)
    runfiles = ctx.runfiles(
        symlinks = {
            ctx.attr.install_path: ctx.file.src,
        },
    )
    return [
        DefaultInfo(
            files = depset([ctx.file.src]),
            runfiles = runfiles,
        ),
        PucConfigInfo(
            install_path = ctx.attr.install_path,
            src = ctx.file.src,
        ),
    ]

puc_config = rule(
    implementation = _puc_config_impl,
    attrs = {
        "install_path": attr.string(
            mandatory = True,
            doc = "Install destination relative to PUC's config root.",
        ),
        "src": attr.label(
            allow_single_file = [".toml"],
            mandatory = True,
            doc = "Repository-owned system-default TOML file.",
        ),
    },
    doc = "Declares an installable TOML file and exposes it at that path in runfiles.",
)
