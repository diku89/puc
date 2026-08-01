# puc

A minimal C reimplementation of the Pi agentic harness for Unix-like operating systems.

## Build and test

The project uses Bazel modules and resolves dependencies from the
[Bazel Central Registry](https://registry.bazel.build/). Install Bazelisk
system-wide, then run:

```sh
git submodule update --init --recursive
bazelisk build //...
bazelisk test //...
```

## Contributors

GitHub requires signed commits. First configure a Git-supported GPG, SSH, or
S/MIME signing key that GitHub recognizes. After cloning, run `./bootstrap.sh`
once. It initializes the pinned submodules, enables recursive Git operations,
automatic commit signing, and the repository's tracked hooks. The hooks keep
submodules synchronized after checkouts, merges, and rebases. A normal
`git commit` will then be signed; `git commit -S` signs an individual commit
explicitly.

Add each contributor's canonical `Name <email>` entry to `AUTHORS`, then map
every exact Git identity they use to that entry in `ALIASES`:

```text
Git Name <git-email> => Canonical Name <canonical-email>
```

The pre-commit hook rejects an unregistered identity or disabled automatic
signing. The pre-push hook rejects newly pushed commits without a signature.
GitHub CI independently validates author registration and signature presence
for every commit introduced by a submitted push or pull request; GitHub's
ruleset performs the authoritative signature verification.
