#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause

"""Enforce that commit authors are registered through ALIASES and AUTHORS."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from typing import Optional, Sequence


def repository_root() -> Path:
    return Path(
        subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            text=True,
        ).strip()
    )


SEPARATOR = " => "
ENTRY_RX = re.compile(r"^.+ <[^<>]+>$")
GIT_IDENT_RX = re.compile(
    r"^(?P<name>.*) <(?P<email>[^>]+)> \d+ [-+]\d+$"
)


def fail(message: str) -> None:
    print(f"check_authors: {message}", file=sys.stderr)


def load_entries(path: Path, kind: str) -> tuple[list[tuple[int, str]], list[str]]:
    if not path.is_file():
        return [], [f"missing {kind} file: {path}"]

    entries: list[tuple[int, str]] = []
    errors: list[str] = []
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        entries.append((lineno, line))
    return entries, errors


def load_policy(
    repo_root: Optional[Path] = None,
) -> tuple[set[str], dict[str, str], list[str]]:
    root = repository_root() if repo_root is None else repo_root
    author_lines, errors = load_entries(root / "AUTHORS", "AUTHORS")
    alias_lines, alias_errors = load_entries(root / "ALIASES", "ALIASES")
    errors.extend(alias_errors)

    authors: set[str] = set()
    for lineno, author in author_lines:
        if not ENTRY_RX.fullmatch(author):
            errors.append(f"malformed AUTHORS line {lineno}: {author!r}")
        if author in authors:
            errors.append(f"duplicate AUTHORS line {lineno}: {author!r}")
        authors.add(author)

    aliases: dict[str, str] = {}
    for lineno, alias in alias_lines:
        if alias.count(SEPARATOR) != 1:
            errors.append(
                f"malformed ALIASES line {lineno} "
                f"(expected one {SEPARATOR.strip()!r}): {alias!r}"
            )
            continue

        identity, canonical = alias.split(SEPARATOR, 1)
        if not ENTRY_RX.fullmatch(identity):
            errors.append(
                f"malformed identity on ALIASES line {lineno}: {identity!r}"
            )
        if not ENTRY_RX.fullmatch(canonical):
            errors.append(
                f"malformed canonical entry on ALIASES line {lineno}: "
                f"{canonical!r}"
            )
        if identity in aliases:
            errors.append(
                f"duplicate identity on ALIASES line {lineno}: {identity!r}"
            )
        aliases[identity] = canonical

        if canonical not in authors:
            errors.append(
                f"ALIASES line {lineno} maps to an entry absent from AUTHORS: "
                f"{canonical!r}"
            )

    return authors, aliases, errors


def pending_identity() -> tuple[str, str]:
    raw = subprocess.check_output(
        ["git", "var", "GIT_AUTHOR_IDENT"],
        text=True,
    ).strip()
    match = GIT_IDENT_RX.fullmatch(raw)
    if match is None:
        raise ValueError(f"could not parse Git author identity: {raw!r}")
    return "pending commit", f"{match.group('name')} <{match.group('email')}>"


def history_identities(revision: str) -> list[tuple[str, str]]:
    log_format = "%H%x1f%an%x1f%ae%x1e"
    raw = subprocess.check_output(
        ["git", "log", f"--format={log_format}", revision],
        text=True,
    )

    identities: list[tuple[str, str]] = []
    for raw_record in raw.split("\x1e"):
        record = raw_record.strip("\n")
        if not record:
            continue
        fields = record.split("\x1f")
        if len(fields) != 3:
            raise ValueError(f"could not parse Git log record: {record!r}")
        commit, name, email = fields
        identities.append((commit, f"{name} <{email}>"))
    return identities


def check_identities(
    identities: list[tuple[str, str]],
    authors: set[str],
    aliases: dict[str, str],
) -> int:
    status = 0
    for source, identity in identities:
        canonical = aliases.get(identity)
        if canonical is None:
            fail(f"{source}: author identity is not in ALIASES: {identity!r}")
            status = 1
        elif canonical not in authors:
            fail(
                f"{source}: ALIASES maps {identity!r} to an AUTHORS entry "
                f"that does not exist: {canonical!r}"
            )
            status = 1
    return status


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    revisions = parser.add_mutually_exclusive_group()
    revisions.add_argument(
        "--all",
        action="store_true",
        help="check every commit reachable from HEAD instead of the pending commit",
    )
    revisions.add_argument(
        "--revision",
        metavar="REVISION",
        help="check commits selected by a git revision or revision range",
    )
    return parser.parse_args(argv)


def main(
    argv: Optional[Sequence[str]] = None,
    repo_root: Optional[Path] = None,
) -> int:
    args = parse_args(argv)
    authors, aliases, errors = load_policy(repo_root)
    for error in errors:
        fail(error)
    if errors:
        return 1

    try:
        if args.all:
            identities = history_identities("HEAD")
        elif args.revision:
            identities = history_identities(args.revision)
        else:
            identities = [pending_identity()]
    except (subprocess.CalledProcessError, ValueError) as error:
        fail(str(error))
        return 2

    return check_identities(identities, authors, aliases)


if __name__ == "__main__":
    sys.exit(main())
