"""Typed failures. A projection failure may follow a committed source event."""
from memory import ScopeError,ConflictError,DeletedScopeError,MemoryErrorBase
from ._core.store import EvidenceViolation,ScopeViolation

class ConfigurationError(ValueError): pass
class ClosedServiceError(RuntimeError): pass
class ThreadOwnershipError(RuntimeError): pass
class ProjectionError(RuntimeError):
    def __init__(self,message,*,source_committed=False,event_id=None):
        self.source_committed=source_committed;self.event_id=event_id
        super().__init__(message)
class PatternGenerationError(RuntimeError): pass
class BudgetExceeded(RuntimeError): pass
