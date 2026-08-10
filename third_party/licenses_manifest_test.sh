#!/usr/bin/env bash

set -euo pipefail

runfiles_root="${TEST_SRCDIR:?}/${TEST_WORKSPACE:?}"

cmp \
  "${runfiles_root}/licenses/manifest.json" \
  "${runfiles_root}/third_party/licenses_manifest.expected.json"
cmp \
  "${runfiles_root}/licenses/libtmt/LICENSE" \
  "${runfiles_root}/third_party/tmt/LICENSE"
cmp \
  "${runfiles_root}/licenses/tomlc17/LICENSE" \
  "${runfiles_root}/third_party/tomlc17/LICENSE"
