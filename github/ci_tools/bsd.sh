#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause

## @file
# @brief Exercise the portable native build on FreeBSD and OpenBSD CI guests.
#
# Bazel's downloaded toolchains do not target these hosts. This script therefore
# checks the portability contract directly with the system compiler, archiver,
# and shell. It deliberately uses POSIX sh rather than Bash because that is the
# common scripting baseline shipped by both operating systems.

## @cond SHELL_IMPLEMENTATION
# Doxygen has no shell parser, so the executable body is intentionally hidden
# from the public symbol index. The source comments below remain the operational
# guide for maintainers reading or changing the script.

set -eu

# Honor CI-provided compiler choices while preserving the native BSD defaults.
cc_command=${CC:-cc}
cxx_command=${CXX:-c++}

# Keep every object and executable outside the checkout. Flat numbered names
# avoid mirroring the source tree and let a single trap remove all build state.
ci_tmp_base=${TMPDIR:-/tmp}
ci_build_dir=$(mktemp -d "${ci_tmp_base%/}/puc-ci.XXXXXX")
trap 'rm -rf "$ci_build_dir"' EXIT HUP INT TERM

# POSIX sh has no arrays. Newline-delimited manifest files provide a portable
# way to accumulate compiler and linker inputs without using eval or word
# splitting on individual paths.
source_list="$ci_build_dir/sources"
object_list="$ci_build_dir/objects"
test_list="$ci_build_dir/tests"
: >"$source_list"
: >"$object_list"

# The portable library convention keeps implementation files under src/. An
# absent directory is valid for an early repository containing only the CLI.
if [ -d src ]; then
    find src -type f \( \
        -name '*.c' -o \
        -name '*.cc' -o \
        -name '*.cpp' -o \
        -name '*.cxx' \
    \) -print | sort >"$source_list"
fi

object_count=0
# Compile language-by-language so a mixed C/C++ library receives the correct
# standard mode while sharing the project's warning-as-error policy.
while IFS= read -r source_file; do
    object_count=$((object_count + 1))
    object_file="$ci_build_dir/source-$object_count.o"
    case "$source_file" in
        *.c)
            "$cc_command" -std=c17 -O2 -Wall -Werror -Iinclude \
                -c "$source_file" -o "$object_file"
            ;;
        *)
            "$cxx_command" -std=c++23 -O2 -Wall -Werror -Iinclude \
                -c "$source_file" -o "$object_file"
            ;;
    esac
    printf '%s\n' "$object_file" >>"$object_list"
done <"$source_list"

library=
# Archive first, then link consumers against one stable library input. Building
# an empty archive is avoided because archiver behavior differs across BSDs.
if [ -s "$object_list" ]; then
    library="$ci_build_dir/libpuc.a"
    set --
    while IFS= read -r object_file; do
        set -- "$@" "$object_file"
    done <"$object_list"
    ar rcs "$library" "$@"
fi

tomlc17_object=
# tomlc17 remains a C translation unit even though its primary consumer is C++.
# Compiling it separately catches accidental reliance on C++ compilation rules.
if [ -f third_party/tomlc17/src/tomlc17.c ]; then
    tomlc17_object="$ci_build_dir/tomlc17.o"
    "$cc_command" -std=c17 -O2 -Wall -Werror \
        -Ithird_party/tomlc17/src \
        -c third_party/tomlc17/src/tomlc17.c -o "$tomlc17_object"
fi

cli_source=
# Accept the common C++ suffixes without encoding one filename choice into the
# BSD harness. The first existing candidate is the project's CLI entry point.
for candidate in puc-cli/main.cc puc-cli/main.cpp puc-cli/main.cxx; do
    if [ -f "$candidate" ]; then
        cli_source=$candidate
        break
    fi
done

if [ -n "$cli_source" ]; then
    # Assemble linker arguments positionally. This preserves each pathname as a
    # single argument and omits optional objects that were not built.
    cli_object="$ci_build_dir/puc-main.o"
    cli_binary="$ci_build_dir/puc"
    "$cxx_command" -std=c++23 -O2 -Wall -Werror \
        -Iinclude -Ithird_party/tomlc17/src \
        -c "$cli_source" -o "$cli_object"
    set -- "$cli_object"
    if [ -n "$library" ]; then
        set -- "$@" "$library"
    fi
    if [ -n "$tomlc17_object" ]; then
        set -- "$@" "$tomlc17_object"
    fi
    "$cxx_command" "$@" -o "$cli_binary"
fi

: >"$test_list"
# Native tests follow the *_test naming contract. They are separate executables
# so one test cannot hide another's unresolved symbols or process-wide state.
if [ -d tests ]; then
    find tests -type f \( \
        -name '*_test.c' -o \
        -name '*_test.cc' -o \
        -name '*_test.cpp' -o \
        -name '*_test.cxx' \
    \) -print | sort >"$test_list"
fi

test_count=0
# Compile tests with the same language standard and warning policy as production
# code, then execute each binary immediately so the failing source is nearby in
# the CI log.
while IFS= read -r test_source; do
    test_count=$((test_count + 1))
    test_object="$ci_build_dir/test-$test_count.o"
    test_binary="$ci_build_dir/test-$test_count"
    case "$test_source" in
        *.c)
            "$cc_command" -std=c17 -O2 -Wall -Werror \
                -Iinclude -Ithird_party/tomlc17/src \
                -c "$test_source" -o "$test_object"
            ;;
        *)
            "$cxx_command" -std=c++23 -O2 -Wall -Werror \
                -Iinclude -Ithird_party/tomlc17/src \
                -c "$test_source" -o "$test_object"
            ;;
    esac
    set -- "$test_object"
    if [ -n "$library" ]; then
        set -- "$@" "$library"
    fi
    if [ -n "$tomlc17_object" ]; then
        set -- "$@" "$tomlc17_object"
    fi
    "$cxx_command" "$@" -o "$test_binary"
    "$test_binary"
done <"$test_list"

if [ "$test_count" -eq 0 ]; then
    # An empty suite is reported rather than treated as an error while the
    # project is still adding portable native tests.
    echo "No native unit tests found; skipping native test execution."
fi
## @endcond
