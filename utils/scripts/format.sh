#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause

## @file
# @brief Apply or verify every repository-owned source formatter.
#
# The default scope mirrors a developer's current change, keeping the edit loop
# quick. ``--full`` supplies the repository-wide invariant used by CI. Paths
# listed in the repository root's ``.formatignore`` are excluded in both modes.
# Tool binaries resolved through Bazel remain pinned by MODULE.bazel;
# clang-format is the only required system formatter.

## @cond SHELL_IMPLEMENTATION
# Doxygen has no shell parser, so the executable body is intentionally hidden
# from the public symbol index. The source comments below remain the operational
# guide for maintainers reading or changing the script.

set -euo pipefail

## @brief Print the command contract shared by help and argument errors.
#
# The usage text lives in one quoted heredoc so shell expansion cannot corrupt
# examples as flags or variables are added later.
usage() {
  cat <<'EOF'
Usage: utils/scripts/format.sh <run|test> [--full]

Commands:
  run       Apply formatting and automatic lint fixes.
  test      Check formatting and lint without modifying files.

Options:
  --full    Process every source file in the repository. By default, only
            files changed since HEAD and untracked files are processed.
  -h, --help
            Show this help text.

Ignore file:
  .formatignore
            Repository-relative Git glob patterns excluded in every mode.
            Blank lines and lines beginning with # are ignored. A trailing /
            excludes the complete directory tree. Negated patterns are not
            supported.
EOF
}

mode=
full=false

# Parse in any order while rejecting ambiguous multiple modes. Exit status 2 is
# reserved for command-line misuse, distinct from a formatter finding changes.
while (($# > 0)); do
  case "$1" in
  run | test)
    if [[ -n "$mode" ]]; then
      echo "format.sh: specify only one command" >&2
      usage >&2
      exit 2
    fi
    mode=$1
    ;;
  --full)
    full=true
    ;;
  -h | --help)
    usage
    exit 0
    ;;
  *)
    echo "format.sh: unknown argument: $1" >&2
    usage >&2
    exit 2
    ;;
  esac
  shift
done

if [[ -z "$mode" ]]; then
  echo "format.sh: provide either 'run' or 'test'" >&2
  usage >&2
  exit 2
fi

repo_root=$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)
cd "$repo_root"

format_pathspecs=(.)
format_ignore_file="$repo_root/.formatignore"
if [[ -f "$format_ignore_file" ]]; then
  while IFS= read -r pattern || [[ -n "$pattern" ]]; do
    pattern=${pattern%$'\r'}
    # Match Git's familiar treatment of surrounding whitespace for this
    # deliberately small, repository-owned configuration format.
    pattern=${pattern#"${pattern%%[![:space:]]*}"}
    pattern=${pattern%"${pattern##*[![:space:]]}"}
    [[ -z "$pattern" || "$pattern" == \#* ]] && continue
    if [[ "$pattern" == \!* ]]; then
      echo ".formatignore: negated patterns are not supported: $pattern" >&2
      exit 2
    fi
    pattern=${pattern#/}
    if [[ -z "$pattern" ]]; then
      echo ".formatignore: patterns must name a path below the repository root" >&2
      exit 2
    fi
    [[ "$pattern" == */ ]] && pattern="${pattern}**"
    format_pathspecs+=(":(exclude,glob)$pattern")
  done <"$format_ignore_file"
fi

files=()
# Git is the source of truth for ownership. NUL delimiters make spaces, tabs,
# and shell metacharacters in filenames safe all the way into Bash arrays. Git
# pathspec exclusions apply .formatignore uniformly to tracked and untracked
# files before language-specific classification.
if [[ "$full" == true ]]; then
  while IFS= read -r -d '' file; do
    files+=("$file")
  done < <(git ls-files --cached --others --exclude-standard -z -- \
    "${format_pathspecs[@]}")
else
  while IFS= read -r -d '' file; do
    files+=("$file")
  done < <(git diff --name-only --diff-filter=ACMRTUXB -z HEAD -- \
    "${format_pathspecs[@]}")
  while IFS= read -r -d '' file; do
    files+=("$file")
  done < <(git ls-files --others --exclude-standard -z -- \
    "${format_pathspecs[@]}")
fi

cc_files=()
bazel_files=()
python_files=()
python_paths=()

# Classify once so each formatter is launched at most once per operation. Files
# deleted since HEAD are ignored because there is no content left to inspect.
for file in "${files[@]}"; do
  [[ -f "$file" ]] || continue

  case "$file" in
  *.c | *.cc | *.cpp | *.cxx | *.h | *.hh | *.hpp | *.hxx)
    cc_files+=("$file")
    ;;
  *.py | *.pyi)
    python_files+=("$file")
    python_paths+=("$repo_root/$file")
    ;;
  BUILD | */BUILD | WORKSPACE | */WORKSPACE | *.bazel | *.bzl)
    bazel_files+=("$file")
    ;;
  esac
done

if ((${#cc_files[@]} > 0)); then
  if ! command -v clang-format >/dev/null 2>&1; then
    echo "format.sh: clang-format is required" >&2
    exit 1
  fi

  # clang-format's dry-run/Werror combination gives test mode the same style
  # decisions as edit mode without relying on a post-format Git diff.
  if [[ "$mode" == run ]]; then
    clang-format -i --style=file -- "${cc_files[@]}"
  else
    clang-format --dry-run --Werror --style=file -- "${cc_files[@]}"
  fi
fi

if ((${#bazel_files[@]} > 0 || ${#python_files[@]} > 0)); then
  # Avoid requiring Bazelisk when a change contains only C/C++ files.
  if ! command -v bazelisk >/dev/null 2>&1; then
    echo "format.sh: bazelisk is required" >&2
    exit 1
  fi
fi

if ((${#bazel_files[@]} > 0)); then
  # Buildifier's lint mode is paired with formatting so BUILD conventions and
  # syntax remain one check rather than two partially overlapping workflows.
  if [[ "$mode" == run ]]; then
    bazelisk run @buildifier_prebuilt//:buildifier -- \
      -mode=fix -lint=fix "${bazel_files[@]}"
  else
    bazelisk run @buildifier_prebuilt//:buildifier -- \
      -mode=check -lint=warn "${bazel_files[@]}"
  fi
fi

if ((${#python_files[@]} > 0)); then
  # Bazel-run tools may start from their external repository. Absolute Python
  # paths ensure Ruff always operates on this checkout rather than that cwd.
  if [[ "$mode" == run ]]; then
    bazelisk run @ruff -- check --fix "${python_paths[@]}"
    bazelisk run @ruff -- format "${python_paths[@]}"
  else
    bazelisk run @ruff -- check "${python_paths[@]}"
    bazelisk run @ruff -- format --check "${python_paths[@]}"
  fi
fi

printf '%s: %d C/C++, %d Bazel, and %d Python file(s)\n' \
  "$mode" "${#cc_files[@]}" "${#bazel_files[@]}" "${#python_files[@]}"
## @endcond
