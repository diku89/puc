#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause

## @file
# @brief Configure a fresh clone to enforce the repository's local policy.
#
# This script is intentionally idempotent so it can be rerun after Git or hook
# changes. It installs no global settings: every Git configuration write is
# scoped to the current repository, leaving the developer's other clones alone.

## @cond SHELL_IMPLEMENTATION
# Doxygen has no shell parser, so the executable body is intentionally hidden
# from the public symbol index. The source comments below remain the operational
# guide for maintainers reading or changing the script.

set -eu

# Resolve through Git so invocation from a subdirectory configures the intended
# work tree and so all following relative policy paths have one stable base.
repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

if ! command -v python3 >/dev/null 2>&1; then
    echo "error: python3 is required by the pre-commit hook" >&2
    exit 1
fi
## @endcond

# These settings make the safe path the default: commits are signed, tracked
# hooks are active, and ordinary Git operations recurse into pinned submodules.
git config core.hooksPath hooks
git config commit.gpgSign true
git config submodule.recurse true
printf 'Configured commit.gpgSign=%s\n' "$(git config --bool commit.gpgSign)"
printf 'Configured core.hooksPath=%s\n' "$(git config core.hooksPath)"
printf 'Configured submodule.recurse=%s\n' \
    "$(git config --bool submodule.recurse)"

echo "Synchronizing Git submodules..."
# Delegate to the same helper used after checkouts, merges, and rebases. This
# keeps first-time setup and later commit navigation on the same code path.
"$repo_root/hooks/sync-submodules"

# Validate the identity Git would actually write, after all Git configuration
# and environment overrides have been applied. Failing here avoids discovering
# an unregistered identity only when the first commit is attempted.
if python3 github/ci_tools/check_authors.py; then
    echo "Current Git author identity is registered."
else
    cat >&2 <<'EOF'

Register your exact Git identity on the left-hand side of ALIASES and map it
to a canonical entry from AUTHORS before committing.
EOF
    exit 1
fi
