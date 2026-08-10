# Vendored libtmt

This directory vendors [`deadpixi/libtmt`](https://github.com/deadpixi/libtmt)
at commit `1da7ba96c459672142949a529e65d730cc5bd6a1` (2019-09-10).

The upstream repository has no standalone license file. Its BSD-3-Clause
license is carried verbatim in `tmt.c`, `tmt.h`, and the License section of
`README.rst`; `LICENSE` reproduces that license in a conventional standalone
form for packaging and audit tooling.

`tmt.c`, `tmt.h`, and `README.rst` are unmodified upstream files. PUC-specific
build integration lives in `//third_party:BUILD`. The target enables upstream's
documented `TMT_HAS_WCWIDTH` option so combining and double-width characters do
not corrupt the one-cell Canvas geometry.
