"""Explicit development launch; real DeepSeek requires --deepseek."""
import argparse
from dataclasses import asdict
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "embedding_module/src"))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("settings", type=Path)
    parser.add_argument("query")
    parser.add_argument("--mode", choices=("fts5", "gemma_dense", "gemma_hybrid"),
                        default="gemma_dense")
    parser.add_argument("--deepseek", action="store_true")
    args = parser.parse_args()
    settings = json.loads(args.settings.read_text(encoding="utf-8-sig"))
    if settings.get("dependencies"):
        sys.path.insert(0, settings["dependencies"])
    import torch
    torch.set_num_threads(4)
    from sigflow_teaching_core.rag.unified import KnowledgeSearch, RetrievalConfig
    config = RetrievalConfig(Path(settings["course_db"]), Path(settings["model_path"]),
                             Path(settings["vector_index"]), args.mode)
    if not args.deepseek:
        result = KnowledgeSearch(config).retrieve(args.query)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return
    from sigflow_teaching_core.deepseek import DeepSeekProvider
    from sigflow_teaching_core.demo import FixtureSource
    from sigflow_teaching_core.domain import RunRequest
    from sigflow_teaching_core.loop import TeachingLoop
    from sigflow_teaching_core.rag.u2_provider import UnifiedCourseProvider
    provider = DeepSeekProvider.from_settings()
    loop = TeachingLoop(
        RunRequest("demo", "r1", "latch", args.query, "explain"), FixtureSource(), provider,
        course_provider=UnifiedCourseProvider(config),
    )
    state = loop.run_until_pause()
    print(json.dumps({"engineering_evidence": "fixture", "real_model": True,
                      "state": asdict(state)}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
