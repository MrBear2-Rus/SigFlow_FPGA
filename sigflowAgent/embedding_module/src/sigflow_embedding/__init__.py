"""Process-local EmbeddingGemma provider, independent of knowledge and memory stores."""
from .provider import GemmaProvider, get_provider

__all__ = ["GemmaProvider", "get_provider"]
