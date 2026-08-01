# SPDX-License-Identifier: BSD-2-Clause

import contextlib
import io
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from github.ci_tools import check_authors


class PolicyTestCase(unittest.TestCase):
    def setUp(self) -> None:
        temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(temporary_directory.cleanup)
        self.repo_root = Path(temporary_directory.name)

    def write_file(self, name: str, contents: str) -> None:
        (self.repo_root / name).write_text(contents, encoding="utf-8")

    def write_valid_policy(self) -> None:
        self.write_file("AUTHORS", "Alice Example <alice@example.com>\n")
        self.write_file(
            "ALIASES",
            "Alice <alice@users.noreply.github.com>"
            " => Alice Example <alice@example.com>\n",
        )


class LoadEntriesTest(PolicyTestCase):
    def test_ignores_comments_blank_lines_and_outer_whitespace(self) -> None:
        path = self.repo_root / "ENTRIES"
        path.write_text(
            "# comment\n\n  Alice Example <alice@example.com>  \n",
            encoding="utf-8",
        )

        entries, errors = check_authors.load_entries(path, "test")

        self.assertEqual([(3, "Alice Example <alice@example.com>")], entries)
        self.assertEqual([], errors)

    def test_reports_a_missing_file(self) -> None:
        path = self.repo_root / "missing"

        entries, errors = check_authors.load_entries(path, "test")

        self.assertEqual([], entries)
        self.assertEqual([f"missing test file: {path}"], errors)


class LoadPolicyTest(PolicyTestCase):
    def test_loads_a_valid_policy(self) -> None:
        self.write_valid_policy()

        authors, aliases, errors = check_authors.load_policy(self.repo_root)

        self.assertEqual({"Alice Example <alice@example.com>"}, authors)
        self.assertEqual(
            {
                "Alice <alice@users.noreply.github.com>":
                    "Alice Example <alice@example.com>"
            },
            aliases,
        )
        self.assertEqual([], errors)

    def test_reports_malformed_and_duplicate_authors(self) -> None:
        self.write_file(
            "AUTHORS",
            "malformed\nAlice <alice@example.com>\nAlice <alice@example.com>\n",
        )
        self.write_file("ALIASES", "")

        _, _, errors = check_authors.load_policy(self.repo_root)

        self.assertIn("malformed AUTHORS line 1: 'malformed'", errors)
        self.assertIn(
            "duplicate AUTHORS line 3: 'Alice <alice@example.com>'",
            errors,
        )

    def test_reports_malformed_and_duplicate_aliases(self) -> None:
        self.write_file("AUTHORS", "Alice <alice@example.com>\n")
        self.write_file(
            "ALIASES",
            "no separator\n"
            "Alice <alias@example.com> => Missing <missing@example.com>\n"
            "Alice <alias@example.com> => Alice <alice@example.com>\n",
        )

        _, _, errors = check_authors.load_policy(self.repo_root)

        self.assertIn(
            "malformed ALIASES line 1 (expected one '=>'): 'no separator'",
            errors,
        )
        self.assertIn(
            "ALIASES line 2 maps to an entry absent from AUTHORS: "
            "'Missing <missing@example.com>'",
            errors,
        )
        self.assertIn(
            "duplicate identity on ALIASES line 3: "
            "'Alice <alias@example.com>'",
            errors,
        )

    def test_reports_malformed_alias_sides(self) -> None:
        self.write_file("AUTHORS", "Alice <alice@example.com>\n")
        self.write_file(
            "ALIASES",
            "invalid identity => invalid canonical\n",
        )

        _, _, errors = check_authors.load_policy(self.repo_root)

        self.assertIn(
            "malformed identity on ALIASES line 1: 'invalid identity'",
            errors,
        )
        self.assertIn(
            "malformed canonical entry on ALIASES line 1: "
            "'invalid canonical'",
            errors,
        )


class GitIdentityTest(unittest.TestCase):
    def test_reads_the_pending_git_identity(self) -> None:
        with mock.patch.object(
            check_authors.subprocess,
            "check_output",
            return_value="Alice Example <alice@example.com> 123 +0000\n",
        ) as check_output:
            identity = check_authors.pending_identity()

        self.assertEqual(
            ("pending commit", "Alice Example <alice@example.com>"),
            identity,
        )
        check_output.assert_called_once_with(
            ["git", "var", "GIT_AUTHOR_IDENT"],
            text=True,
        )

    def test_rejects_an_unparseable_pending_identity(self) -> None:
        with mock.patch.object(
            check_authors.subprocess,
            "check_output",
            return_value="not a git identity",
        ):
            with self.assertRaisesRegex(ValueError, "could not parse"):
                check_authors.pending_identity()

    def test_reads_identities_from_a_revision(self) -> None:
        git_output = (
            "abc123\x1fAlice Example\x1falice@example.com\x1e\n"
            "def456\x1fBob Example\x1fbob@example.com\x1e\n"
        )
        with mock.patch.object(
            check_authors.subprocess,
            "check_output",
            return_value=git_output,
        ) as check_output:
            identities = check_authors.history_identities("base..head")

        self.assertEqual(
            [
                ("abc123", "Alice Example <alice@example.com>"),
                ("def456", "Bob Example <bob@example.com>"),
            ],
            identities,
        )
        check_output.assert_called_once_with(
            [
                "git",
                "log",
                "--format=%H%x1f%an%x1f%ae%x1e",
                "base..head",
            ],
            text=True,
        )

    def test_rejects_a_malformed_history_record(self) -> None:
        with mock.patch.object(
            check_authors.subprocess,
            "check_output",
            return_value="commit\x1fname only\x1e",
        ):
            with self.assertRaisesRegex(ValueError, "could not parse"):
                check_authors.history_identities("HEAD")


class CheckIdentitiesTest(unittest.TestCase):
    def test_accepts_registered_identities(self) -> None:
        canonical = "Alice Example <alice@example.com>"

        status = check_authors.check_identities(
            [("abc123", "Alice <alias@example.com>")],
            {canonical},
            {"Alice <alias@example.com>": canonical},
        )

        self.assertEqual(0, status)

    def test_reports_unregistered_and_invalid_mappings(self) -> None:
        errors = io.StringIO()
        with contextlib.redirect_stderr(errors):
            status = check_authors.check_identities(
                [
                    ("abc123", "Unknown <unknown@example.com>"),
                    ("def456", "Alice <alias@example.com>"),
                ],
                set(),
                {
                    "Alice <alias@example.com>":
                        "Alice Example <alice@example.com>"
                },
            )

        self.assertEqual(1, status)
        self.assertIn("author identity is not in ALIASES", errors.getvalue())
        self.assertIn("AUTHORS entry that does not exist", errors.getvalue())


class MainTest(PolicyTestCase):
    def test_checks_the_pending_identity_by_default(self) -> None:
        self.write_valid_policy()
        with mock.patch.object(
            check_authors,
            "pending_identity",
            return_value=(
                "pending commit",
                "Alice <alice@users.noreply.github.com>",
            ),
        ) as pending_identity:
            status = check_authors.main([], self.repo_root)

        self.assertEqual(0, status)
        pending_identity.assert_called_once_with()

    def test_checks_an_explicit_revision(self) -> None:
        self.write_valid_policy()
        with mock.patch.object(
            check_authors,
            "history_identities",
            return_value=[
                ("abc123", "Alice <alice@users.noreply.github.com>")
            ],
        ) as history_identities:
            status = check_authors.main(
                ["--revision", "base..head"],
                self.repo_root,
            )

        self.assertEqual(0, status)
        history_identities.assert_called_once_with("base..head")

    def test_reports_policy_errors_before_reading_git(self) -> None:
        self.write_file("AUTHORS", "malformed\n")
        self.write_file("ALIASES", "")
        errors = io.StringIO()

        with contextlib.redirect_stderr(errors):
            with mock.patch.object(check_authors, "pending_identity") as pending:
                status = check_authors.main([], self.repo_root)

        self.assertEqual(1, status)
        pending.assert_not_called()
        self.assertIn("malformed AUTHORS", errors.getvalue())

    def test_reports_git_failures(self) -> None:
        self.write_valid_policy()
        error = subprocess.CalledProcessError(1, ["git", "var"])
        errors = io.StringIO()

        with contextlib.redirect_stderr(errors):
            with mock.patch.object(
                check_authors,
                "pending_identity",
                side_effect=error,
            ):
                status = check_authors.main([], self.repo_root)

        self.assertEqual(2, status)
        self.assertIn("returned non-zero exit status 1", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
