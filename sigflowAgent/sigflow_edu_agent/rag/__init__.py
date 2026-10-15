"""Small local course knowledge base for the SigFlow education agent."""

__all__ = ["render_citations", "retrieve_course_passages", "validate_citations"]


def __getattr__(name: str):
    if name == "retrieve_course_passages":
        from .retriever import retrieve_course_passages

        return retrieve_course_passages
    if name in {"render_citations", "validate_citations"}:
        from . import citations

        return getattr(citations, name)
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
