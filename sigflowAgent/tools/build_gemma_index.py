"""Build a new course-only index; student history never enters this command."""
import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "embedding_module/src"))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--course-db", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--dependencies", type=Path)
    args = parser.parse_args()
    if args.dependencies:
        sys.path.insert(0, str(args.dependencies))
    from sigflow_teaching_core.rag.gemma_index import build_index
    print(build_index(args.course_db, args.model, args.output))


if __name__ == "__main__":
    main()
