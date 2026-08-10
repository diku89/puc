load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_shell//shell:sh_test.bzl", "sh_test")
load(":licenses.bzl", "puc_license", "puc_license_manifest")

exports_files([
    "googletest_1_17_0_char8_t.patch",
    "licenses.bzl",
    "tmt/LICENSE",
    "tmt/README.puc.md",
    "tmt/README.rst",
])

puc_license(
    name = "tomlc17_license",
    license_file = "tomlc17/LICENSE",
    manifest_file_entry = {
        "license_file": "licenses/tomlc17/LICENSE",
        "repository": "https://github.com/cktan/tomlc17",
    },
)

puc_license(
    name = "tmt_license",
    license_file = "tmt/LICENSE",
    manifest_file_entry = {
        "license_file": "licenses/libtmt/LICENSE",
        "repository": "https://github.com/deadpixi/libtmt",
    },
)

puc_license_manifest(
    name = "licenses",
    license_declarations = [
        ":tmt_license",
        ":tomlc17_license",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "tmt",
    srcs = ["tmt/tmt.c"],
    hdrs = ["tmt/tmt.h"],
    copts = [
        "-std=c99",
        "-Wno-unused-variable",
    ],
    local_defines = ["TMT_HAS_WCWIDTH"],
    strip_include_prefix = "tmt",
    visibility = ["//visibility:public"],
)

sh_test(
    name = "licenses_manifest_test",
    size = "small",
    srcs = ["licenses_manifest_test.sh"],
    data = [
        "licenses_manifest.expected.json",
        ":licenses",
    ],
)

cc_library(
    name = "tomlc17",
    srcs = ["tomlc17/src/tomlc17.c"],
    hdrs = ["tomlc17/src/tomlc17.h"],
    copts = ["-std=c17"],
    strip_include_prefix = "tomlc17/src",
    visibility = ["//visibility:public"],
)
