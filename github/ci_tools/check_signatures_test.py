# SPDX-License-Identifier: BSD-2-Clause

"""Test signature-presence policy without requiring real signing keys.

Fixtures model raw Git commit objects and the pre-push stdin protocol directly.
Cryptographic verification is intentionally absent because production delegates
that responsibility to GitHub; these tests guard the offline presence check and
the revision ranges selected before a push.
"""

import contextlib
import io
import subprocess
import unittest
from unittest import mock

from github.ci_tools import check_signatures


class NullOidTest(unittest.TestCase):
    """Keep hook sentinels independent of Git's configured hash length."""

    def test_recognizes_null_oids_of_any_length(self) -> None:
        self.assertTrue(check_signatures.is_null_oid("0" * 40))
        self.assertTrue(check_signatures.is_null_oid("0" * 64))

    def test_rejects_empty_and_nonzero_oids(self) -> None:
        self.assertFalse(check_signatures.is_null_oid(""))
        self.assertFalse(check_signatures.is_null_oid("0001"))


class GitObjectTest(unittest.TestCase):
    """Distinguish real signature headers from similar commit-message text."""

    def test_reads_commits_from_git_revisions(self) -> None:
        with mock.patch.object(
            check_signatures.subprocess,
            "check_output",
            return_value="abc123\ndef456\n",
        ) as check_output:
            commits = check_signatures.revision_commits(["base..head"])

        self.assertEqual(["abc123", "def456"], commits)
        check_output.assert_called_once_with(
            ["git", "rev-list", "base..head"],
            text=True,
        )

    def test_detects_standard_signature_headers(self) -> None:
        commit_object = (
            "tree abc123\n"
            "gpgsig -----BEGIN SSH SIGNATURE-----\n"
            " signature body\n"
            " -----END SSH SIGNATURE-----\n"
            "\nsubject\n"
        )
        with mock.patch.object(
            check_signatures.subprocess,
            "check_output",
            return_value=commit_object,
        ):
            self.assertTrue(check_signatures.commit_has_signature("abc123"))

    def test_detects_sha256_signature_headers(self) -> None:
        commit_object = (
            "tree abc123\n"
            "gpgsig-sha256 -----BEGIN PGP SIGNATURE-----\n"
            " signature body\n"
            "\nsubject\n"
        )
        with mock.patch.object(
            check_signatures.subprocess,
            "check_output",
            return_value=commit_object,
        ):
            self.assertTrue(check_signatures.commit_has_signature("abc123"))

    def test_rejects_signatures_mentioned_only_in_the_message(self) -> None:
        commit_object = (
            "tree abc123\nauthor Alice <alice@example.com> 123 +0000\n"
            "\ngpgsig not actually a header\n"
        )
        with mock.patch.object(
            check_signatures.subprocess,
            "check_output",
            return_value=commit_object,
        ):
            self.assertFalse(check_signatures.commit_has_signature("abc123"))


class PrePushTest(unittest.TestCase):
    """Verify ref updates select only commits newly introduced to the remote."""

    def test_builds_revision_sets_for_updated_and_new_refs(self) -> None:
        lines = io.StringIO(
            "refs/heads/main local1 refs/heads/main remote1\n"
            "refs/heads/topic local2 refs/heads/topic 00000000\n"
            "refs/heads/old 00000000 refs/heads/old remote3\n"
        )

        revision_sets = check_signatures.pre_push_revision_sets(lines, "origin")

        self.assertEqual(
            [
                ["remote1..local1"],
                ["local2", "--not", "--remotes=origin"],
            ],
            revision_sets,
        )

    def test_rejects_malformed_hook_input(self) -> None:
        with self.assertRaisesRegex(ValueError, "pre-push input line 1"):
            check_signatures.pre_push_revision_sets(
                ["not enough fields\n"],
                "origin",
            )


class CheckCommitsTest(unittest.TestCase):
    """Ensure batch enforcement reports all repairable commits in one run."""

    def test_accepts_signed_commits(self) -> None:
        with mock.patch.object(
            check_signatures,
            "commit_has_signature",
            return_value=True,
        ):
            self.assertEqual(0, check_signatures.check_commits(["abc123"]))

    def test_reports_every_unsigned_commit(self) -> None:
        errors = io.StringIO()
        with contextlib.redirect_stderr(errors):
            with mock.patch.object(
                check_signatures,
                "commit_has_signature",
                side_effect=lambda commit: commit == "signed",
            ):
                status = check_signatures.check_commits(
                    ["signed", "unsigned1", "unsigned2"]
                )

        self.assertEqual(1, status)
        self.assertIn("unsigned commit: unsigned1", errors.getvalue())
        self.assertIn("unsigned commit: unsigned2", errors.getvalue())


class MainTest(unittest.TestCase):
    """Exercise direct and hook modes plus their operational failure status."""

    def test_checks_an_explicit_revision(self) -> None:
        with mock.patch.object(
            check_signatures,
            "revision_commits",
            return_value=["abc123"],
        ) as revision_commits:
            with mock.patch.object(
                check_signatures,
                "commit_has_signature",
                return_value=True,
            ):
                status = check_signatures.main(["base..head"])

        self.assertEqual(0, status)
        revision_commits.assert_called_once_with(["base..head"])

    def test_checks_refs_from_pre_push_input(self) -> None:
        hook_input = io.StringIO("refs/heads/main local refs/heads/main remote\n")
        with mock.patch.object(
            check_signatures,
            "revision_commits",
            return_value=["abc123"],
        ) as revision_commits:
            with mock.patch.object(
                check_signatures,
                "commit_has_signature",
                return_value=True,
            ):
                status = check_signatures.main(
                    ["--pre-push", "origin"],
                    hook_input,
                )

        self.assertEqual(0, status)
        revision_commits.assert_called_once_with(["remote..local"])

    def test_reports_git_failures(self) -> None:
        error = subprocess.CalledProcessError(1, ["git", "rev-list"])
        errors = io.StringIO()
        with contextlib.redirect_stderr(errors):
            with mock.patch.object(
                check_signatures,
                "revision_commits",
                side_effect=error,
            ):
                status = check_signatures.main(["HEAD"])

        self.assertEqual(2, status)
        self.assertIn("returned non-zero exit status 1", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
