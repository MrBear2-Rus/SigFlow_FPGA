"""Inspect the current database without creating or modifying files."""
import argparse
import json
from pathlib import Path

from .database import configured_database
from .retriever import retrieve_course_passages


def main():
    parser = argparse.ArgumentParser(description='查询当前课程资料库')
    parser.add_argument('--db', type=Path, default=None)
    parser.add_argument('--course-id')
    parser.add_argument('--language', choices=('zh-CN', 'en'))
    parser.add_argument('--top-k', type=int, choices=(1, 2, 3), default=3)
    parser.add_argument('question', nargs='?')
    args = parser.parse_args()
    while True:
        try:
            question = args.question or input('课程问题（q 退出）> ').strip()
        except EOFError:
            break
        if question in {'q', 'quit', 'exit'}:
            break
        result = retrieve_course_passages(question, course_id=args.course_id, language=args.language,
                                          top_k=args.top_k, db_path=args.db or configured_database())
        print(json.dumps(result, ensure_ascii=False, indent=2))
        if args.question:
            break


if __name__ == '__main__':
    main()
