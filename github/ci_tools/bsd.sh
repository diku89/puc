#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause

set -eu

cc_command=${CC:-cc}
cxx_command=${CXX:-c++}
ci_tmp_base=${TMPDIR:-/tmp}
ci_build_dir=$(mktemp -d "${ci_tmp_base%/}/puc-ci.XXXXXX")
trap 'rm -rf "$ci_build_dir"' EXIT HUP INT TERM

source_list="$ci_build_dir/sources"
object_list="$ci_build_dir/objects"
test_list="$ci_build_dir/tests"
: >"$source_list"
: >"$object_list"

if [ -d src ]; then
    find src -type f \( \
        -name '*.c' -o \
        -name '*.cc' -o \
        -name '*.cpp' -o \
        -name '*.cxx' \
    \) -print | sort >"$source_list"
fi

object_count=0
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
if [ -s "$object_list" ]; then
    library="$ci_build_dir/libpuc.a"
    set --
    while IFS= read -r object_file; do
        set -- "$@" "$object_file"
    done <"$object_list"
    ar rcs "$library" "$@"
fi

tomlc17_object=
if [ -f third_party/tomlc17/src/tomlc17.c ]; then
    tomlc17_object="$ci_build_dir/tomlc17.o"
    "$cc_command" -std=c17 -O2 -Wall -Werror \
        -Ithird_party/tomlc17/src \
        -c third_party/tomlc17/src/tomlc17.c -o "$tomlc17_object"
fi

cli_source=
for candidate in puc-cli/main.cc puc-cli/main.cpp puc-cli/main.cxx; do
    if [ -f "$candidate" ]; then
        cli_source=$candidate
        break
    fi
done

if [ -n "$cli_source" ]; then
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
if [ -d tests ]; then
    find tests -type f \( \
        -name '*_test.c' -o \
        -name '*_test.cc' -o \
        -name '*_test.cpp' -o \
        -name '*_test.cxx' \
    \) -print | sort >"$test_list"
fi

test_count=0
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
    echo "No native unit tests found; skipping native test execution."
fi
