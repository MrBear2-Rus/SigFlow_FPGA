"""Small dependency-free JSON Schema validator for the frozen v1 contracts.

The project has no third-party runtime dependencies, so tests cannot rely on
``jsonschema``. This validator implements the JSON Schema keywords actually
used by ``contracts/edu-agent/v1/schemas``: local/external ``$ref``, objects,
arrays, scalar types, required/additional properties, enums, const, patterns,
length/item bounds and ``oneOf``.
"""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any, Mapping, Sequence


class SchemaValidationError(ValueError):
    """Raised when a document does not satisfy a frozen schema."""


class ContractSchemaValidator:
    """Load and validate the JSON schemas shipped with the v1 contracts."""

    _MAX_REF_DEPTH = 64

    def __init__(self, schema_directory: str | Path) -> None:
        self.schema_directory = Path(schema_directory)
        self._cache: dict[Path, Mapping[str, Any]] = {}

    def validate(
        self,
        document: Any,
        schema_name: str,
        pointer: str | None = None,
    ) -> None:
        """Validate *document* against one schema file or an internal pointer."""
        schema_path = self._schema_path(schema_name)
        schema = self._load(schema_path)
        if pointer:
            schema = self._resolve_pointer(schema, pointer, schema_path)
        self._validate(document, schema, "$", schema_path, 0)

    def schema_names(self) -> list[str]:
        """Return every schema filename in deterministic order."""
        return sorted(path.name for path in self.schema_directory.glob("*.schema.json"))

    def _schema_path(self, name: str) -> Path:
        path = (self.schema_directory / name).resolve()
        try:
            path.relative_to(self.schema_directory.resolve())
        except ValueError:
            raise SchemaValidationError("schema path escapes the contract directory")
        if not path.is_file():
            raise SchemaValidationError("unknown schema file: %s" % name)
        return path

    def _load(self, path: Path) -> Mapping[str, Any]:
        resolved = path.resolve()
        if resolved not in self._cache:
            try:
                value = json.loads(resolved.read_text(encoding="utf-8"))
            except (OSError, ValueError) as exc:
                raise SchemaValidationError("cannot load schema %s: %s" % (resolved.name, exc))
            if not isinstance(value, Mapping):
                raise SchemaValidationError("schema root must be an object: %s" % resolved.name)
            self._cache[resolved] = value
        return self._cache[resolved]

    def _resolve_ref(
        self,
        ref: str,
        current_path: Path,
    ) -> tuple[Mapping[str, Any], Path]:
        file_part, separator, fragment = ref.partition("#")
        target_path = current_path if not file_part else self._schema_path(file_part)
        target = self._load(target_path)
        if separator and fragment:
            target = self._resolve_pointer(target, "#" + fragment, target_path)
        return target, target_path

    def _resolve_pointer(
        self,
        root: Mapping[str, Any],
        pointer: str,
        schema_path: Path,
    ) -> Mapping[str, Any]:
        if pointer == "#":
            return root
        if not pointer.startswith("#/"):
            raise SchemaValidationError("unsupported schema pointer: %s" % pointer)
        current: Any = root
        for raw_part in pointer[2:].split("/"):
            part = raw_part.replace("~1", "/").replace("~0", "~")
            if isinstance(current, Mapping):
                if part not in current:
                    raise SchemaValidationError(
                        "missing schema pointer %s in %s" % (pointer, schema_path.name)
                    )
                current = current[part]
            elif isinstance(current, list):
                try:
                    current = current[int(part)]
                except (ValueError, IndexError):
                    raise SchemaValidationError(
                        "missing schema pointer %s in %s" % (pointer, schema_path.name)
                    )
            else:
                raise SchemaValidationError("schema pointer crosses a scalar")
        if not isinstance(current, Mapping):
            raise SchemaValidationError("schema pointer does not resolve to an object")
        return current

    def _validate(
        self,
        value: Any,
        schema: Mapping[str, Any],
        path: str,
        schema_path: Path,
        depth: int,
    ) -> None:
        if depth > self._MAX_REF_DEPTH:
            raise SchemaValidationError("schema reference depth exceeded at %s" % path)

        ref = schema.get("$ref")
        if isinstance(ref, str):
            target, target_path = self._resolve_ref(ref, schema_path)
            self._validate(value, target, path, target_path, depth + 1)
            return

        one_of = schema.get("oneOf")
        if isinstance(one_of, list):
            matches = 0
            branch_errors: list[str] = []
            for branch in one_of:
                if not isinstance(branch, Mapping):
                    raise SchemaValidationError("oneOf branch must be an object")
                try:
                    self._validate(value, branch, path, schema_path, depth + 1)
                    matches += 1
                except SchemaValidationError as exc:
                    branch_errors.append(str(exc))
            if matches != 1:
                raise SchemaValidationError(
                    "%s must match exactly one oneOf branch (matched %d; %s)"
                    % (path, matches, " | ".join(branch_errors))
                )
            return

        expected_type = schema.get("type")
        if expected_type is not None and not self._matches_type(value, expected_type):
            raise SchemaValidationError(
                "%s expected type %r, got %s" % (path, expected_type, type(value).__name__)
            )

        if "const" in schema and value != schema["const"]:
            raise SchemaValidationError("%s does not equal const %r" % (path, schema["const"]))
        enum = schema.get("enum")
        if isinstance(enum, list) and value not in enum:
            raise SchemaValidationError("%s value %r is not in enum" % (path, value))

        if isinstance(value, str):
            minimum_length = schema.get("minLength")
            maximum_length = schema.get("maxLength")
            if isinstance(minimum_length, int) and len(value) < minimum_length:
                raise SchemaValidationError("%s is shorter than minLength" % path)
            if isinstance(maximum_length, int) and len(value) > maximum_length:
                raise SchemaValidationError("%s is longer than maxLength" % path)
            pattern = schema.get("pattern")
            if isinstance(pattern, str) and re.search(pattern, value) is None:
                raise SchemaValidationError("%s does not match pattern %r" % (path, pattern))

        if self._is_number(value):
            minimum = schema.get("minimum")
            maximum = schema.get("maximum")
            if self._is_number(minimum) and value < minimum:
                raise SchemaValidationError("%s is below minimum" % path)
            if self._is_number(maximum) and value > maximum:
                raise SchemaValidationError("%s is above maximum" % path)

        if isinstance(value, list):
            minimum_items = schema.get("minItems")
            maximum_items = schema.get("maxItems")
            if isinstance(minimum_items, int) and len(value) < minimum_items:
                raise SchemaValidationError("%s has fewer than minItems" % path)
            if isinstance(maximum_items, int) and len(value) > maximum_items:
                raise SchemaValidationError("%s has more than maxItems" % path)
            item_schema = schema.get("items")
            if isinstance(item_schema, Mapping):
                for index, item in enumerate(value):
                    self._validate(
                        item,
                        item_schema,
                        "%s[%d]" % (path, index),
                        schema_path,
                        depth + 1,
                    )

        if isinstance(value, Mapping):
            required = schema.get("required", [])
            if isinstance(required, Sequence) and not isinstance(required, (str, bytes)):
                for name in required:
                    if isinstance(name, str) and name not in value:
                        raise SchemaValidationError("%s is missing required field %r" % (path, name))
            properties = schema.get("properties")
            property_map = properties if isinstance(properties, Mapping) else {}
            for name, child_schema in property_map.items():
                if name in value and isinstance(child_schema, Mapping):
                    self._validate(
                        value[name],
                        child_schema,
                        "%s.%s" % (path, name),
                        schema_path,
                        depth + 1,
                    )
            additional = schema.get("additionalProperties", True)
            extras = set(value) - set(property_map)
            if additional is False and extras:
                raise SchemaValidationError(
                    "%s has unexpected fields: %s" % (path, ", ".join(sorted(map(str, extras))))
                )
            if isinstance(additional, Mapping):
                for name in extras:
                    self._validate(
                        value[name],
                        additional,
                        "%s.%s" % (path, name),
                        schema_path,
                        depth + 1,
                    )

    @staticmethod
    def _matches_type(value: Any, expected: Any) -> bool:
        if isinstance(expected, list):
            return any(ContractSchemaValidator._matches_type(value, item) for item in expected)
        if expected == "object":
            return isinstance(value, Mapping)
        if expected == "array":
            return isinstance(value, list)
        if expected == "string":
            return isinstance(value, str)
        if expected == "integer":
            return type(value) is int
        if expected == "number":
            return ContractSchemaValidator._is_number(value)
        if expected == "boolean":
            return type(value) is bool
        if expected == "null":
            return value is None
        raise SchemaValidationError("unsupported schema type: %r" % expected)

    @staticmethod
    def _is_number(value: Any) -> bool:
        return type(value) in (int, float)
