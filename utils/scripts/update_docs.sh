#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause

## @file
# @brief Regenerate the checked-in Markdown code reference transactionally.
#
# Doxygen XML and Markdown are built in a staging directory under docs/. The
# existing docs/code tree is not moved until conversion and link validation
# finish, so interruption or malformed documentation cannot leave a partial
# reference tree in the checkout.

## @cond SHELL_IMPLEMENTATION
# Doxygen has no shell parser, so the executable body is intentionally hidden
# from the public symbol index. The source comments below remain the operational
# guide for maintainers reading or changing the script.

set -euo pipefail

repo_root=$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)
cd "$repo_root"

if ! command -v bazelisk >/dev/null 2>&1; then
  echo "update_docs.sh: bazelisk is required" >&2
  exit 1
fi

mkdir -p "$repo_root/docs"
# Place staging beside the destination so the final rename stays on one
# filesystem and is atomic on the supported Unix hosts.
temp_dir=$(mktemp -d "$repo_root/docs/.update-docs.XXXXXX")
generated_dir="$repo_root/docs/code"
previous_dir="$temp_dir/previous-code"
restore_previous=false

## @brief Remove staging state and restore the previous tree after a failed swap.
#
# Bazel outputs are read-only. Making the copied XML writable is necessary for
# reliable cleanup on both macOS and GNU userlands. The restoration guard is
# true only during the narrow interval between moving old and new trees.
cleanup() {
  if [[ "$restore_previous" == true && -d "$previous_dir" && ! -e "$generated_dir" ]]; then
    mv "$previous_dir" "$generated_dir"
  fi
  chmod -R u+w "$temp_dir" 2>/dev/null || true
  rm -rf -- "$temp_dir"
}
trap cleanup EXIT

bazelisk build //docs:code_xml

# Ask Bazel for its configured output directory instead of assuming the
# workspace convenience symlink exists or has its default name.
xml_output="$(bazelisk info bazel-bin)/docs/xml"

if [[ ! -d "$xml_output" ]]; then
  echo "update_docs.sh: could not locate the Doxygen XML output" >&2
  exit 1
fi

# Never hand the converter files inside Bazel's cache. A private copy isolates
# later cleanup and prevents any converter behavior from mutating build outputs.
cp -R "$xml_output" "$temp_dir/xml"
chmod -R u+w "$temp_dir/xml"
bazelisk run //utils/scripts:doxygen_xml_to_markdown -- \
  "$temp_dir/xml" "$temp_dir/code"

# Refuse surprising destination types before any move. In particular, replacing
# a symlink could write outside the repository-owned documentation directory.
if [[ -L "$generated_dir" || ( -e "$generated_dir" && ! -d "$generated_dir" ) ]]; then
  echo "update_docs.sh: docs/code must be a directory" >&2
  exit 1
fi

# Keep the previous complete tree inside staging until the new tree is installed.
# The EXIT trap can put it back if the second move fails.
if [[ -d "$generated_dir" ]]; then
  mv "$generated_dir" "$previous_dir"
  restore_previous=true
fi
mv "$temp_dir/code" "$generated_dir"
restore_previous=false

echo "Updated docs/code"
## @endcond
