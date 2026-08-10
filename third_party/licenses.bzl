"""Declare third-party licenses and assemble their install manifest.

`puc_license` is the license counterpart to `puc_config`: its DefaultInfo
exposes the repository-owned license at the manifest entry's install path in
runfiles, while PucLicenseInfo preserves the source file and structured entry
for a future packaging rule.

Example:

```starlark
puc_license(
    name = "example_license",
    license_file = "example/LICENSE",
    manifest_file_entry = {
        "repository": "https://example.invalid/owner/repository",
        "license_file": "licenses/example/LICENSE",
    },
)
```

`puc_license_manifest` collects declarations into deterministic, human-readable
JSON at `licenses/manifest.json`. A future install package can include the
aggregate target in exactly the same way tests currently consume `puc_config`
targets through `data`.
"""

PucLicenseInfo = provider(
    doc = "One third-party license and its repository-to-installed-file mapping.",
    fields = {
        "license_file": "The repository-owned source license File.",
        "manifest_file_entry": "A struct containing repository and license_file strings.",
    },
)

PucLicenseManifestInfo = provider(
    doc = "A generated aggregate of installable PUC third-party licenses.",
    fields = {
        "install_path": "The manifest path relative to the install root.",
        "manifest_file": "The generated JSON manifest File.",
    },
)

_MANIFEST_KEYS = ["license_file", "repository"]
_MANIFEST_INSTALL_PATH = "licenses/manifest.json"

def _validate_relative_file(path, description):
    """Reject non-canonical or directory-valued install paths."""
    if not path:
        fail("%s must not be empty" % description)
    if path.startswith("/") or path.endswith("/"):
        fail("%s must be relative and name a file: %s" % (description, path))
    if "\\" in path:
        fail("%s must use '/' separators: %s" % (description, path))
    for component in path.split("/"):
        if not component or component == "." or component == "..":
            fail("%s must be a canonical relative path: %s" % (description, path))

def _validated_manifest_file_entry(entry):
    """Validate and normalize the two fields serialized for one dependency."""
    if sorted(entry.keys()) != _MANIFEST_KEYS:
        fail("manifest_file_entry must contain exactly 'repository' and 'license_file'")

    repository = entry["repository"]
    if not repository or repository != repository.strip():
        fail("manifest_file_entry.repository must be nonempty with no outer whitespace")
    if "\n" in repository or "\r" in repository or "\t" in repository:
        fail("manifest_file_entry.repository must occupy one printable line")

    license_path = entry["license_file"]
    _validate_relative_file(license_path, "manifest_file_entry.license_file")
    if not license_path.startswith("licenses/"):
        fail("manifest_file_entry.license_file must be below licenses/: %s" % license_path)
    if license_path == _MANIFEST_INSTALL_PATH:
        fail("a dependency license cannot replace %s" % _MANIFEST_INSTALL_PATH)

    return struct(
        license_file = license_path,
        repository = repository,
    )

def _puc_license_impl(ctx):
    entry = _validated_manifest_file_entry(ctx.attr.manifest_file_entry)
    return [
        DefaultInfo(
            files = depset([ctx.file.license_file]),
            runfiles = ctx.runfiles(
                symlinks = {
                    entry.license_file: ctx.file.license_file,
                },
            ),
        ),
        PucLicenseInfo(
            license_file = ctx.file.license_file,
            manifest_file_entry = entry,
        ),
    ]

puc_license = rule(
    implementation = _puc_license_impl,
    attrs = {
        "license_file": attr.label(
            allow_single_file = True,
            mandatory = True,
            doc = "Repository-owned license text for the dependency.",
        ),
        "manifest_file_entry": attr.string_dict(
            mandatory = True,
            doc = "Repository identifier and install-root-relative license_file mapping.",
        ),
    },
    doc = "Declares one installable third-party license and manifest mapping.",
)

def _manifest_json(entries_by_repository):
    """Render stable, readable JSON without relying on dictionary ordering."""
    repositories = sorted(entries_by_repository.keys())
    lines = [
        "{",
        "  \"format_version\": 1,",
        "  \"licenses\": [",
    ]
    for index, repository in enumerate(repositories):
        entry = entries_by_repository[repository]
        lines.extend([
            "    {",
            "      \"repository\": %s," % json.encode(repository),
            "      \"license_file\": %s" % json.encode(entry.license_file),
            "    }%s" % ("," if index + 1 < len(repositories) else ""),
        ])
    lines.extend([
        "  ]",
        "}",
        "",
    ])
    return "\n".join(lines)

def _puc_license_manifest_impl(ctx):
    entries_by_repository = {}
    repositories_by_path = {}
    symlinks = {}
    transitive_files = []
    for target in ctx.attr.licenses:
        info = target[PucLicenseInfo]
        entry = info.manifest_file_entry
        if entry.repository in entries_by_repository:
            fail("duplicate license repository: %s" % entry.repository)
        if entry.license_file in repositories_by_path:
            fail("license install path %s is shared by %s and %s" % (
                entry.license_file,
                repositories_by_path[entry.license_file],
                entry.repository,
            ))
        entries_by_repository[entry.repository] = entry
        repositories_by_path[entry.license_file] = entry.repository
        symlinks[entry.license_file] = info.license_file
        transitive_files.append(target[DefaultInfo].files)

    manifest = ctx.actions.declare_file(ctx.label.name + ".json")
    ctx.actions.write(
        output = manifest,
        content = _manifest_json(entries_by_repository),
    )
    symlinks[_MANIFEST_INSTALL_PATH] = manifest
    return [
        DefaultInfo(
            files = depset([manifest], transitive = transitive_files),
            runfiles = ctx.runfiles(symlinks = symlinks),
        ),
        PucLicenseManifestInfo(
            install_path = _MANIFEST_INSTALL_PATH,
            manifest_file = manifest,
        ),
    ]

puc_license_manifest = rule(
    implementation = _puc_license_manifest_impl,
    attrs = {
        "licenses": attr.label_list(
            mandatory = True,
            providers = [[PucLicenseInfo]],
            doc = "License declarations to validate, install, and serialize.",
        ),
    },
    doc = "Builds deterministic JSON and the complete installed license runfiles tree.",
)
