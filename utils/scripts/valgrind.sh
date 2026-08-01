#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause

## @file
# @brief Run one Bazel test executable under Valgrind Memcheck.
#
# Bazel's `--run_under` option invokes this script with the test
# executable and its arguments. Keeping the Memcheck policy here makes local
# and CI runs identical, and avoids embedding fragile shell quoting in
# `.bazelrc`.
#
# Only definite and indirect leaks fail the test. Reachable allocations can be
# owned legitimately by process-wide runtimes at exit, but they remain visible
# in the report for review. `--track-origins=yes` trades additional runtime for
# the allocation or stack site responsible for an undefined value.

## @cond SHELL_IMPLEMENTATION
# Doxygen has no shell parser, so the executable body is intentionally hidden
# from the public symbol index. The file documentation above remains visible.

set -eu

if [ "$#" -eq 0 ]; then
    echo "usage: valgrind.sh TEST_EXECUTABLE [ARGUMENT ...]" >&2
    exit 2
fi

exec valgrind \
    --tool=memcheck \
    --track-origins=yes \
    --leak-check=full \
    --show-leak-kinds=all \
    --errors-for-leak-kinds=definite,indirect \
    --error-exitcode=1 \
    "$@"

## @endcond
