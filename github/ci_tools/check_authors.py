#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause

"""Enforce the repository's canonical contributor identity policy.

``AUTHORS`` records the public identity by which a contributor wants to be
known. ``ALIASES`` maps the exact name and email stored in Git commits to one
of those canonical entries. Keeping those concerns separate lets contributors
use several machines or historical Git identities without creating duplicate
people in the public contributor list.

The hook and CI entry points share this module so local feedback and protected
branch enforcement interpret the policy identically. Policy violations return
1; failures to inspect Git return 2 so callers can distinguish bad data from a
broken execution environment.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from typing import Optional, Sequence


def repository_root() -> Path:
    """Locate policy files relative to Git rather than the caller's directory.

    Hooks are not guaranteed to run with the repository root as their working
    directory. Asking Git for the root keeps direct invocation, hooks, and CI
    behavior consistent.

    @return Absolute path to the active work tree's root.
    @throws subprocess.CalledProcessError If the command is outside a work tree.
    """
    return Path(
        subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            text=True,
        ).strip()
    )


## Delimiter chosen to keep both sides human-readable in ALIASES.
SEPARATOR = " => "

## Minimal shared grammar for canonical entries and exact Git identities.
ENTRY_RX = re.compile(r"^.+ <[^<>]+>$")

## Git identity grammar excluding the timestamp and timezone suffix.
GIT_IDENT_RX = re.compile(r"^(?P<name>.*) <(?P<email>[^>]+)> \d+ [-+]\d+$")


def fail(message: str) -> None:
    """Emit a diagnostic with a stable tool prefix for hooks and CI logs.

    @param message Human-readable explanation without a program-name prefix.
    """
    print(f"check_authors: {message}", file=sys.stderr)


def load_entries(path: Path, kind: str) -> tuple[list[tuple[int, str]], list[str]]:
    """Read significant policy lines while retaining their source locations.

    Parsing and validation are deliberately separate. Returning line numbers
    here allows ``load_policy`` to report every structural problem in one run,
    rather than forcing a contributor through one edit-and-retry cycle per
    error.

    @param path Policy file to read as UTF-8 text.
    @param kind Display name used when the file is missing.
    @return ``(entries, errors)`` where entries contain original line numbers.
    """
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
    """Build and cross-check the canonical-author and alias tables.

    The function accumulates all syntax, duplication, and dangling-reference
    errors before returning. In particular, an alias is not considered valid
    merely because its left side parses: its canonical right side must be an
    exact entry in ``AUTHORS``.

    @param repo_root Repository containing ``AUTHORS`` and ``ALIASES``. Tests
        pass an isolated directory; production callers let Git discover it.
    @return Canonical authors, exact alias mappings, and all policy errors.
    """
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
            errors.append(f"malformed identity on ALIASES line {lineno}: {identity!r}")
        if not ENTRY_RX.fullmatch(canonical):
            errors.append(
                f"malformed canonical entry on ALIASES line {lineno}: {canonical!r}"
            )
        if identity in aliases:
            errors.append(f"duplicate identity on ALIASES line {lineno}: {identity!r}")
        aliases[identity] = canonical

        if canonical not in authors:
            errors.append(
                f"ALIASES line {lineno} maps to an entry absent from AUTHORS: "
                f"{canonical!r}"
            )

    return authors, aliases, errors


def pending_identity() -> tuple[str, str]:
    """Read the identity Git would write into a new commit.

    ``git var GIT_AUTHOR_IDENT`` applies Git's complete configuration and
    environment precedence. Reimplementing that resolution in Python could
    validate a different identity from the one the following commit records.

    @return A source label and normalized ``Name <email>`` identity.
    @throws subprocess.CalledProcessError If Git cannot resolve the identity.
    @throws ValueError If Git returns an unexpected identity representation.
    """
    raw = subprocess.check_output(
        ["git", "var", "GIT_AUTHOR_IDENT"],
        text=True,
    ).strip()
    match = GIT_IDENT_RX.fullmatch(raw)
    if match is None:
        raise ValueError(f"could not parse Git author identity: {raw!r}")
    return "pending commit", f"{match.group('name')} <{match.group('email')}>"


def history_identities(revision: str) -> list[tuple[str, str]]:
    """Extract commit identities from an arbitrary Git revision expression.

    ASCII unit and record separators avoid ambiguous parsing when a person's
    name contains spaces or punctuation. The commit hash is retained as the
    source label so a CI failure points directly to the commit needing repair.

    @param revision Any revision or range accepted by ``git log``.
    @return ``(commit, identity)`` pairs in Git's traversal order.
    @throws subprocess.CalledProcessError If Git cannot resolve the revision.
    @throws ValueError If a supposedly machine-delimited record is malformed.
    """
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
    """Validate identities without stopping after the first bad commit.

    Reporting the whole range matters for a pull request containing several
    commits: contributors can rewrite every offending commit in one rebase.

    @param identities Source labels paired with exact Git identities.
    @param authors Canonical public identities loaded from ``AUTHORS``.
    @param aliases Exact Git-identity to canonical-identity mappings.
    @return 0 when every identity resolves to an author, otherwise 1.
    """
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
    """Select pending-commit, complete-history, or revision-range checking.

    @param argv Explicit argument vector for tests, or ``None`` for ``sys.argv``.
    @return Parsed command-line namespace.
    """
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
    """Load policy before touching history, then validate the requested scope.

    Policy errors take precedence because identity results cannot be trusted
    until the mapping itself is internally consistent. Operational Git errors
    use a distinct exit status from ordinary policy rejection.

    @param argv Optional argument vector used by tests and the command line.
    @param repo_root Optional policy root used to isolate unit tests from the
        developer's checkout.
    @return 0 for success, 1 for policy rejection, or 2 for Git/parse failure.
    """
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
