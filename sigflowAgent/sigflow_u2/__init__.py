"""Source-tree U2 entry point, independent of U1's ``sigflow_edu_agent``.

The wheel maps this namespace directly to the implementation directory.  This
source-only bridge uses the same files without importing or aliasing U1's name.
"""
from pathlib import Path as _Path

__path__ = [str(_Path(__file__).resolve().parent.parent / 'sigflow_edu_agent')]
