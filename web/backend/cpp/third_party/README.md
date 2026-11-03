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

Upstream: https://github.com/tree-sitter/tree-sitter and
https://github.com/tree-sitter/tree-sitter-verilog

No configure/build step downloads dependencies.
