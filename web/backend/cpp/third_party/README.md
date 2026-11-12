# Offline dependencies

`nlohmann/json.hpp` was copied from the existing desktop workspace's
`3rd/nlohmann/json.hpp`. This is a vendored build input, not an external build
path. The upstream copyright and MIT license notice remain in the header.
Upstream: https://github.com/nlohmann/json
SHA-256: `5f09d1eebe9b3557f21df155869af13bcde79b74a0748f82ca946e8cc088aaa0`

Tree-sitter runtime and generated Verilog grammar were copied from the user's
existing `SigFlow_FPGA_Cmake/3rd` workspace. The local snapshot did not contain
upstream revision metadata or root license files; the accompanying MIT license
texts were restored from the current upstream repositories. The exact upstream
revision of this snapshot remains to be established. Its generated parser has
SHA-256 `3d06921742cbdc0d5b1c88d7f8ed986eb09ae422c625dc9b47f941e7076a91c9`.
Unicode license notices already included in the runtime are preserved.

Local compatibility evidence (not exact release identifiers): the generated
Verilog parser declares language ABI 14 (`STATE_COUNT=20534`,
`SYMBOL_COUNT=1202`); the vendored runtime header supports language ABI 13–15.
`api.h` SHA-256 is
`06fb92656fdfda49f9566d1f76b3444dc931e2aff57c97e17503f43193bb2804`.
These values establish the parser/runtime ABI range but do not identify the
runtime release, grammar tag, or source commit. The Unicode subset records its
own ICU source commit in `tree-sitter/lib/src/unicode/ICU_SHA`.

An upstream release cross-check on 2026-11-11 found that the generated parser
in tags `v1.0.1`, `v1.0.2`, and `v1.0.3` has SHA-256
`fe014cd25643bea9e380cc358c126359574387f420d9a6e71c1699fc83bfb822`
(46,381,071 bytes), which does not match this snapshot. The checked release
sources are available at
https://raw.githubusercontent.com/tree-sitter/tree-sitter-verilog/v1.0.1/src/parser.c,
https://raw.githubusercontent.com/tree-sitter/tree-sitter-verilog/v1.0.2/src/parser.c,
and
https://raw.githubusercontent.com/tree-sitter/tree-sitter-verilog/v1.0.3/src/parser.c.
This rules out those exact parser files as the source of the vendored grammar;
it does not identify the snapshot's actual revision.

Runtime source cross-check on 2026-11-12: after normalizing CRLF/LF, 43 of 44
vendored code/header files under `lib/include` and `lib/src` match the official
`v0.26.1` source tree; `lib/src/unicode/ICU_SHA` also matches. The remaining
`lib/src/wasm/wasm-stdlib.h` differs, and its local fingerprint did not match
the official tags from `v0.25.0` through `v0.27.2`. `api.h` content is shared
by tags `v0.26.1` through `v0.26.7`. This is strong evidence for a `v0.26.1`
runtime baseline, but the exception and nonmatching grammar prevent claiming
that the vendored directory is an unmodified checkout of that tag. Reference:
https://github.com/tree-sitter/tree-sitter/tree/v0.26.1 and
https://github.com/tree-sitter/tree-sitter/tree/v0.26.7.

Upstream: https://github.com/tree-sitter/tree-sitter and
https://github.com/tree-sitter/tree-sitter-verilog

No configure/build step downloads dependencies.
