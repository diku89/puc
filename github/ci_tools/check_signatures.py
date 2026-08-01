#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause

"""Reject commits that lack an embedded Git commit signature.

This checker verifies signature *presence*, not cryptographic trust. Local Git
does not necessarily have the public keys that GitHub knows about, so hooks and
CI inspect the raw commit object for its signature header while the GitHub
ruleset remains authoritative for verification. Keeping those responsibilities
separate gives fast offline feedback without pretending to reproduce GitHub's
key and identity policy.

The pre-push path understands Git's four-field hook protocol and checks only
commits newly introduced to the selected remote. The direct path accepts normal
``git rev-list`` expressions for CI and diagnostics.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from typing import Iterable, Optional, Sequence, TextIO

## Raw commit header accepted for SHA-1 and SHA-256 object formats.
SIGNATURE_HEADER_RX = re.compile(r"^gpgsig(?:-sha256)? ", re.MULTILINE)


def fail(message: str) -> None:
    """Write a consistently prefixed diagnostic to standard error.

    @param message Explanation suitable for a hook or CI log.
    """
    print(f"check_signatures: {message}", file=sys.stderr)


def is_null_oid(oid: str) -> bool:
    """Recognize Git's all-zero sentinel without assuming a hash algorithm.

    Git installations may use SHA-1 or SHA-256 object IDs. Testing the content
    rather than a fixed length handles both formats and shortened fixtures.

    @param oid Object ID field from Git's hook protocol.
    @return Whether the field is nonempty and contains only zeroes.
    """
    return bool(oid) and not oid.strip("0")


def revision_commits(revisions: Sequence[str]) -> list[str]:
    """Delegate revision-set semantics to Git and return concrete commits.

    Passing each expression as its own argument preserves compound sets such
    as ``tip --not --remotes=origin`` without invoking a shell.

    @param revisions Arguments accepted by ``git rev-list``.
    @return Commit object IDs selected by the revision set.
    @throws subprocess.CalledProcessError If Git rejects the revision set.
    """
    output = subprocess.check_output(
        ["git", "rev-list", *revisions],
        text=True,
    )
    return [line for line in output.splitlines() if line]


def commit_has_signature(commit: str) -> bool:
    """Inspect only commit headers for a standard or SHA-256 signature field.

    Partitioning the raw object at its first blank line prevents commit-message
    text containing ``gpgsig`` from being mistaken for an actual signature.

    @param commit Commit object ID or revision resolvable by ``git cat-file``.
    @return Whether the commit header contains an embedded signature.
    @throws subprocess.CalledProcessError If the object cannot be read.
    """
    commit_object = subprocess.check_output(
        ["git", "cat-file", "commit", commit],
        text=True,
    )
    headers, _, _ = commit_object.partition("\n\n")
    return SIGNATURE_HEADER_RX.search(headers) is not None


def pre_push_revision_sets(
    lines: Iterable[str],
    remote_name: str,
) -> list[list[str]]:
    """Translate pre-push ref updates into the commits newly reaching a remote.

    Updated refs are simple ``old..new`` ranges. A newly created ref has no old
    tip, so it is compared with every commit already reachable from that remote;
    otherwise an existing signed history would be needlessly rechecked. Deleted
    refs have no local tip and introduce no commits.

    @param lines Four-field records supplied to a Git pre-push hook on stdin.
    @param remote_name Remote whose existing reachability should be excluded.
    @return A list of argument vectors suitable for ``git rev-list``.
    @throws ValueError If any hook record does not have exactly four fields.
    """
    revision_sets: list[list[str]] = []
    for lineno, line in enumerate(lines, 1):
        fields = line.split()
        if len(fields) != 4:
            raise ValueError(
                f"could not parse pre-push input line {lineno}: {line.rstrip()!r}"
            )

        _, local_oid, _, remote_oid = fields
        if is_null_oid(local_oid):
            continue
        if is_null_oid(remote_oid):
            revision_sets.append([local_oid, "--not", f"--remotes={remote_name}"])
        else:
            revision_sets.append([f"{remote_oid}..{local_oid}"])
    return revision_sets


def unique_commits(revision_sets: Iterable[Sequence[str]]) -> list[str]:
    """Resolve and de-duplicate commits while retaining deterministic order.

    One commit may reach several pushed refs. A dictionary is used as an
    insertion-ordered set so it is inspected and diagnosed only once.

    @param revision_sets Revision argument vectors produced for each ref.
    @return Unique commit IDs in first-seen order.
    @throws subprocess.CalledProcessError If a revision set cannot be resolved.
    """
    commits: dict[str, None] = {}
    for revisions in revision_sets:
        for commit in revision_commits(revisions):
            commits.setdefault(commit, None)
    return list(commits)


def check_commits(commits: Iterable[str]) -> int:
    """Report every unsigned commit in a batch.

    @param commits Commit IDs to inspect.
    @return 0 when all commits contain signatures, otherwise 1.
    @throws subprocess.CalledProcessError If a commit object cannot be read.
    """
    status = 0
    for commit in commits:
        if not commit_has_signature(commit):
            fail(f"unsigned commit: {commit}")
            status = 1
    return status


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    """Enforce the mutually exclusive hook-input and direct-revision modes.

    @param argv Explicit arguments for tests, or ``None`` for ``sys.argv``.
    @return Validated command-line namespace.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--pre-push",
        metavar="REMOTE_NAME",
        help="read refs from the Git pre-push hook for this remote",
    )
    parser.add_argument(
        "revisions",
        nargs="*",
        help="git revisions accepted by git rev-list",
    )
    args = parser.parse_args(argv)
    if args.pre_push and args.revisions:
        parser.error("revisions cannot be combined with --pre-push")
    if not args.pre_push and not args.revisions:
        parser.error("provide a revision or use --pre-push")
    return args


def main(
    argv: Optional[Sequence[str]] = None,
    stdin: Optional[TextIO] = None,
) -> int:
    """Resolve the requested commit set and enforce signature presence.

    @param argv Optional command-line arguments.
    @param stdin Injectable pre-push stream; defaults to process stdin.
    @return 0 for success, 1 for unsigned commits, or 2 for malformed input or
        a Git inspection failure.
    """
    args = parse_args(argv)
    input_stream = sys.stdin if stdin is None else stdin

    try:
        if args.pre_push:
            revision_sets = pre_push_revision_sets(input_stream, args.pre_push)
        else:
            revision_sets = [args.revisions]
        commits = unique_commits(revision_sets)
        return check_commits(commits)
    except (subprocess.CalledProcessError, ValueError) as error:
        fail(str(error))
        return 2


if __name__ == "__main__":
    sys.exit(main())
