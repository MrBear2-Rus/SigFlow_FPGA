"""UA-10 public API; import performs no network, model load or U2 initialization."""
from memory import LearningEvent,JobEvidence,Scope,LearningService,CourseInteraction,U2TeachingFacts
from .config import MemoryConfig
from .schemas import (Observation,TeachingTask,WriteResult,LearningFact,LearningPattern,QueryResult,
    PatternResult,TeachingMemoryBundle,RevocationResult,RelatedAttempt,RelatedAttemptsResult)
from .service import MemoryHost,MemoryService
from .teaching import TeachingMemory,TeachingContext
from . import errors

__version__='0.3.0.dev0'
__all__=['TeachingMemory','TeachingContext','MemoryHost','MemoryService','MemoryConfig','LearningService','LearningEvent','JobEvidence','Scope',
    'Observation','TeachingTask','WriteResult','LearningFact','LearningPattern','QueryResult','PatternResult','TeachingMemoryBundle','RevocationResult',
    'CourseInteraction','U2TeachingFacts','errors']
