# puc

A minimal C++ reimplementation of the Pi agentic harness for Unix-like operating systems.

## Build and test

The project uses Bazel modules and resolves dependencies from the
[Bazel Central Registry](https://registry.bazel.build/). Install Bazelisk
system-wide, then run:

```sh
git submodule update --init --recursive
bazelisk build //...
bazelisk test //...
```

Project sources are compiled as C++23 with `-Wall -Werror`. Production is the
default build mode. The same full build and test suite can be run under Clang's
sanitizers with:

```sh
bazelisk build --config=asan //...
bazelisk test --config=asan //...
bazelisk build --config=tsan //...
bazelisk test --config=tsan //...
```

On Linux, install Valgrind and run the native C++ tests under Memcheck:

```sh
sudo apt-get install valgrind
bazelisk test --config=valgrind \
  $(bazelisk query 'kind("cc_test rule", //...)')
```

The Valgrind configuration enables origin tracking and full leak checking.
Definite or indirect leaks and all other Memcheck errors fail the test. GitHub
CI discovers `cc_test` targets with the same query, so newly added native tests
are covered automatically without running Python interpreters under Valgrind.

## Formatting and linting

Install `clang-format` and Bazelisk, then use the repository formatter to apply
C/C++, Bazel, and Python formatting and automatic lint fixes:

```sh
utils/scripts/format.sh run
```

By default, the script processes only files changed since the last commit plus
untracked files. Use `--full` to process the entire repository:

```sh
utils/scripts/format.sh run --full
```

Use `test` to check the same files without modifying them. This exits nonzero
if formatting or lint fixes are required:

```sh
utils/scripts/format.sh test
utils/scripts/format.sh test --full
```

`clang-format` formats C and C++ sources. Buildifier and Ruff are resolved
through Bazel's pinned module dependencies and format/lint Bazel and Python
sources respectively. GitHub CI runs `test --full` on every push and pull
request, so all repository-owned source files must be clean before merging.

## Code documentation

API documentation is generated from Doxygen comments in the C++ sources. Run:

```sh
utils/scripts/update_docs.sh
```

Bazel downloads the pinned Doxygen toolchain, generates temporary XML, and
converts it into linked Markdown pages under `docs/code/`. Commit those generated
pages alongside the source changes that affect them. GitHub CI repeats the
generation on Ubuntu and fails if the checked-in pages are stale.

When the `Check generated code documentation` job fails, regenerate and review
the pages, then stage them with the source change:

```sh
utils/scripts/update_docs.sh
git diff -- docs/code
git add docs/code
```

Channels, typed messages, and numeric message IDs are collected into one
generated [PUC Protocol Contracts](docs/code/puc_protocol_contracts.md) page
with structured custom commands:

```cpp
/// \channel{//screen/events||Purpose text||Producer symbols||Consumer symbols}
/// \msg{puc::msg::Event||Purpose text||Producer symbols||Consumer symbols}
/// \msg_id{42||puc::msg::Event||Purpose text||Producer symbols||Consumer symbols}
```

The arguments use `||` as their separator, so normal commas remain available
in prose. Use Doxygen `\ref` commands in producer and consumer arguments when a
named C++ symbol should be linked. Every entry must state its purpose and both
sides of the contract; this keeps the generated list useful as an architectural
inventory rather than merely a symbol index.

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
