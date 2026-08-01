#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause

# Configure the tracked Git hooks for a fresh clone. This is idempotent.

set -eu

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

if ! command -v python3 >/dev/null 2>&1; then
    echo "error: python3 is required by the pre-commit hook" >&2
    exit 1
fi

git config commit.gpgSign true
git config core.hooksPath hooks
printf 'Configured commit.gpgSign=%s\n' "$(git config --bool commit.gpgSign)"
printf 'Configured core.hooksPath=%s\n' "$(git config core.hooksPath)"

if python3 github/ci_tools/check_authors.py; then
    echo "Current Git author identity is registered."
else
    cat >&2 <<'EOF'

Register your exact Git identity on the left-hand side of ALIASES and map it
to a canonical entry from AUTHORS before committing.
EOF
    exit 1
fi
