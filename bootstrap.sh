#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause

# Configure a fresh clone for development. This is idempotent.

set -eu

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

if ! command -v python3 >/dev/null 2>&1; then
    echo "error: python3 is required by the pre-commit hook" >&2
    exit 1
fi

git config commit.gpgSign true
git config core.hooksPath hooks
git config submodule.recurse true
printf 'Configured commit.gpgSign=%s\n' "$(git config --bool commit.gpgSign)"
printf 'Configured core.hooksPath=%s\n' "$(git config core.hooksPath)"
printf 'Configured submodule.recurse=%s\n' \
    "$(git config --bool submodule.recurse)"

echo "Synchronizing Git submodules..."
"$repo_root/hooks/sync-submodules"

if python3 github/ci_tools/check_authors.py; then
    echo "Current Git author identity is registered."
else
    cat >&2 <<'EOF'

Register your exact Git identity on the left-hand side of ALIASES and map it
to a canonical entry from AUTHORS before committing.
EOF
    exit 1
fi
