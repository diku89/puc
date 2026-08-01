#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause

"""Reject commits that do not contain a Git commit signature."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from typing import Iterable, Optional, Sequence, TextIO

SIGNATURE_HEADER_RX = re.compile(r"^gpgsig(?:-sha256)? ", re.MULTILINE)


def fail(message: str) -> None:
    print(f"check_signatures: {message}", file=sys.stderr)


def is_null_oid(oid: str) -> bool:
    return bool(oid) and not oid.strip("0")


def revision_commits(revisions: Sequence[str]) -> list[str]:
    output = subprocess.check_output(
        ["git", "rev-list", *revisions],
        text=True,
    )
    return [line for line in output.splitlines() if line]


def commit_has_signature(commit: str) -> bool:
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
            revision_sets.append(
                [local_oid, "--not", f"--remotes={remote_name}"]
            )
        else:
            revision_sets.append([f"{remote_oid}..{local_oid}"])
    return revision_sets


def unique_commits(revision_sets: Iterable[Sequence[str]]) -> list[str]:
    commits: dict[str, None] = {}
    for revisions in revision_sets:
        for commit in revision_commits(revisions):
            commits.setdefault(commit, None)
    return list(commits)


def check_commits(commits: Iterable[str]) -> int:
    status = 0
    for commit in commits:
        if not commit_has_signature(commit):
            fail(f"unsigned commit: {commit}")
            status = 1
    return status


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
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
