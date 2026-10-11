"""Pure wire-contract adapters; none confer execution authorization."""
from .plan import ApprovalPackage, build_plan_card

__all__ = ['ApprovalPackage', 'build_plan_card']
