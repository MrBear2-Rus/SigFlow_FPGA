# Offline dependencies

`nlohmann/json.hpp` was copied from the existing desktop workspace's
`3rd/nlohmann/json.hpp`. This is a vendored build input, not an external build
path. The upstream copyright and MIT license notice remain in the header.
Upstream: https://github.com/nlohmann/json
SHA-256: `5f09d1eebe9b3557f21df155869af13bcde79b74a0748f82ca946e8cc088aaa0`

No configure/build step downloads dependencies. Other dependencies in the
architecture are not migrated in this batch.
