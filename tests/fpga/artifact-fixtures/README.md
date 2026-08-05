# Yosys JSON artifact fixtures

These fixtures define the C03 validation contract for `ArtifactValidator`.

| Fixture | Expected status |
| --- | --- |
| `valid/top.json` | `Valid` for top module `top` |
| `empty/top.json` | `TooSmall` |
| `truncated/top.json` | `InvalidJson` |
| `invalid-json/top.json` | `InvalidJson` |

The valid fixture deliberately contains the Yosys JSON fields required by C03:
`modules`, the requested top module, and object-valued `ports`, `cells`, and
`netnames`.
