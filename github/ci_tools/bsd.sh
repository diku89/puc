#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause

set -eu

cc_command=${CC:-cc}
ci_tmp_base=${TMPDIR:-/tmp}
ci_build_dir=$(mktemp -d "${ci_tmp_base%/}/puc-ci.XXXXXX")
trap 'rm -rf "$ci_build_dir"' EXIT HUP INT TERM

source_list="$ci_build_dir/sources"
object_list="$ci_build_dir/objects"
test_list="$ci_build_dir/tests"
: >"$source_list"
: >"$object_list"

if [ -d src ]; then
    find src -type f -name '*.c' -print | sort >"$source_list"
fi

object_count=0
while IFS= read -r source_file; do
    object_count=$((object_count + 1))
    object_file="$ci_build_dir/source-$object_count.o"
    "$cc_command" -std=c11 -Wall -Wextra -Werror -Iinclude \
        -c "$source_file" -o "$object_file"
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

: >"$test_list"
if [ -d tests ]; then
    find tests -type f -name '*_test.c' -print | sort >"$test_list"
fi

test_count=0
while IFS= read -r test_source; do
    test_count=$((test_count + 1))
    test_binary="$ci_build_dir/test-$test_count"
    "$cc_command" -std=c11 -Wall -Wextra -Werror -Iinclude \
        "$test_source" ${library:+"$library"} -o "$test_binary"
    "$test_binary"
done <"$test_list"

if [ "$test_count" -eq 0 ]; then
    echo "No native C tests found; skipping native test execution."
fi
