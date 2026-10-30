from dataclasses import dataclass
from pathlib import Path
from typing import Literal
from .errors import ConfigurationError

@dataclass(frozen=True)
class MemoryConfig:
    data_dir: Path
    embedding_model_path: Path | None = None
    backend: Literal['simple','lightmem'] = 'simple'
    default_token_budget: int = 1024
    enable_real_api: bool = False
    allow_experimental_lightmem: bool = False

    def __post_init__(self):
        if type(self.enable_real_api) is not bool or type(self.allow_experimental_lightmem) is not bool:
            raise ConfigurationError('enablement switches require explicit booleans')
        object.__setattr__(self,'data_dir',Path(self.data_dir).expanduser().resolve())
        if self.embedding_model_path is not None:
            model=Path(self.embedding_model_path).expanduser().resolve()
            if not model.is_dir():raise ConfigurationError('embedding model directory does not exist; downloading is disabled')
            object.__setattr__(self,'embedding_model_path',model)
        if self.backend not in ('simple','lightmem'):raise ConfigurationError('unknown backend')
        if self.backend=='lightmem' and not self.allow_experimental_lightmem:raise ConfigurationError('LightMem is experimental and requires explicit enablement')
        if type(self.default_token_budget) is not int or not 128<=self.default_token_budget<=2048:raise ConfigurationError('token budget must be an integer in 128..2048')
