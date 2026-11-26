"""Explicit local model contract and lazy, serialized CPU inference."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
from threading import RLock
from typing import TYPE_CHECKING
from importlib.metadata import version

if TYPE_CHECKING:
    import numpy as np

DIMENSION = 768
QUERY_PROMPT = "task: search result | query: "
DOCUMENT_FORMAT = "title: {title} | text: {content}"
RUNTIME_VERSIONS = {'sentence-transformers': '6.2.0', 'transformers': '5.22.1',
                    'torch': '2.8.0', 'numpy': '2.2.6',
                    'tokenizers': '0.23.3', 'safetensors': '0.8.0'}
_providers: dict[Path, GemmaProvider] = {}
_registry_lock = RLock()


def validate_runtime_versions(installed: dict[str, str]) -> None:
    """Contract v2 supports this verified implementation profile, including CPU torch wheels."""
    for name, expected in RUNTIME_VERSIONS.items():
        if installed.get(name, '').split('+', 1)[0] != expected:
            raise ValueError('Unsupported Gemma runtime: ' + name + '; use the tested constraints')


def file_digest(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def asset_paths(directory: Path) -> list[Path]:
    extensions = {'.json', '.txt', '.model', '.jinja', '.safetensors', '.bin',
                  '.py', '.yaml', '.yml', '.tiktoken', '.vocab'}
    return [path for path in sorted(directory.rglob('*'))
            if path.is_file() and '__pycache__' not in path.parts and path.suffix in extensions]


def encoding_assets(directory: Path) -> dict[str, str]:
    """Fingerprint all local configuration/tokenizer assets, including module subdirectories."""
    assets = {}
    for path in asset_paths(directory):
        assets[path.relative_to(directory).as_posix()] = file_digest(path)
    required = {"config.json", "modules.json", "tokenizer_config.json",
                "config_sentence_transformers.json", "tokenizer.json", "model.safetensors",
                "1_Pooling/config.json"}
    if not required <= assets.keys():
        raise ValueError("Incomplete local Gemma model assets")
    return assets


class GemmaProvider:
    """One model per resolved model directory; callers own their separate indexes."""

    def __init__(self, model_path: Path):
        self.model_path = model_path.resolve()
        self._model = None
        self.load_count = 0
        self._lock = RLock()
        pooling = json.loads((self.model_path / "1_Pooling/config.json").read_text())
        if pooling != {"embedding_dimension": 768, "pooling_mode": "mean",
                       "include_prompt": True}:
            raise ValueError("Gemma requires mean pooling")
        self.contract = {
            "contract_version": 2,
            "assets": encoding_assets(self.model_path),
            "text_only_config": {"vision_config": None, "audio_config": None},
            "dtype": "float32",
            "model_id": "google/embeddinggemma-2",
            "source_weight_sha256": file_digest(self.model_path / "model.safetensors"),
            "tokenizer_sha256": file_digest(self.model_path / "tokenizer.json"),
            "pooling": {"embedding_dimension": 768, "pooling_mode": "mean",
                        "include_prompt": True},
            "query_prompt": QUERY_PROMPT,
            "document_format": DOCUMENT_FORMAT,
            "dimension": DIMENSION,
            "max_length": 8192,
            "normalization": "L2",
        }
        sentence_config = json.loads(
            (self.model_path / "config_sentence_transformers.json").read_text(encoding="utf-8")
        )
        if sentence_config.get("default_prompt_name") is not None:
            raise ValueError("Expected no default document prompt; explicit templates are used")
        if sentence_config.get("prompts", {}).get("SearchQuery") != QUERY_PROMPT:
            raise ValueError("Unexpected configured SearchQuery template")
        self.contract["default_prompt_name"] = None
        self._asset_stats = self._current_stats()
        encoded = json.dumps(self.contract, sort_keys=True).encode()
        self.fingerprint = hashlib.sha256(encoded).hexdigest()

    def _load(self) -> None:
        self.validate_assets()
        if self._model is not None:
            return
        validate_runtime_versions({name: version(name) for name in RUNTIME_VERSIONS})
        from sentence_transformers import SentenceTransformer

        model = SentenceTransformer(
            str(self.model_path),
            config_kwargs={"vision_config": None, "audio_config": None},
            device="cpu",
            local_files_only=True,
            model_kwargs={"dtype": "float32"},
        )
        if model.prompts.get("SearchQuery") != QUERY_PROMPT:
            raise ValueError("Unexpected SearchQuery template")
        if model.get_sentence_embedding_dimension() != DIMENSION:
            raise ValueError("Unexpected embedding dimension")
        model.max_seq_length = 8192
        self._model = model
        self.load_count += 1

    def _current_stats(self) -> dict[str, tuple[int, int]]:
        return {path.relative_to(self.model_path).as_posix():
                (path.stat().st_size, path.stat().st_mtime_ns) for path in asset_paths(self.model_path)}

    def validate_assets(self) -> None:
        if self._current_stats() != self._asset_stats:
            raise ValueError("Model assets changed: restart and explicitly rebuild/validate the index")

    def encode_queries(self, texts: list[str]) -> np.ndarray:
        """Return normalized float32 query vectors using the official SearchQuery task."""
        return self._encode(texts, "SearchQuery")

    def encode_documents(
        self, texts: list[str], titles: list[str] | None = None,
    ) -> np.ndarray:
        """Use the validated title/text recipe; metadata never replaces original evidence."""
        titles = titles if titles is not None else [""] * len(texts)
        if len(titles) != len(texts):
            raise ValueError("One title is required per document")
        documents = []
        for title, content in zip(titles, texts):
            documents.append(DOCUMENT_FORMAT.format(title=title, content=content))
        return self._encode(documents, None)

    def _encode(self, texts: list[str], prompt: str | None) -> np.ndarray:
        import numpy as np

        if not texts:
            return np.empty((0, DIMENSION), dtype=np.float32)
        with self._lock:
            self.validate_assets()
            self._load()
            vectors = self._model.encode(
                texts, prompt_name=prompt, batch_size=1,
                normalize_embeddings=True, show_progress_bar=False,
                convert_to_numpy=True,
            )
        if vectors.shape != (len(texts), DIMENSION) or not np.isfinite(vectors).all():
            raise ValueError("Invalid Gemma vectors")
        return vectors.astype(np.float32)

    def count_tokens(self, text: str) -> int:
        with self._lock:
            self._load()
            return len(self._model.tokenizer.encode(text, add_special_tokens=False))

    def warm_up(self) -> None:
        """Explicit development startup step before U2's bounded history read deadline."""
        self.encode_queries(["检索预热"])


def get_provider(model_path: Path) -> GemmaProvider:
    """Reuse only within this Python process; no cross-process sharing is implied."""
    path = Path(model_path).resolve()
    with _registry_lock:
        if path not in _providers:
            _providers[path] = GemmaProvider(path)
        _providers[path].validate_assets()
        return _providers[path]
