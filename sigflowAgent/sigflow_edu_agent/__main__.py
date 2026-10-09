"""Development CLI. --demo never contacts DeepSeek or executes EDA."""

import argparse
from dataclasses import asdict
import json
import sys
from uuid import uuid4

from .deepseek import DeepSeekProvider
from .demo import FixtureSource, run_demo
from .domain import Plan, ProviderError, RunRequest, Status
from .loop import TeachingLoop


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description='U2 教学主循环开发入口（示例证据）')
    parser.add_argument('--demo', action='store_true', help='离线演示五条循环路径，不调用 API')
    parser.add_argument('--provider', choices=('deepseek', 'rule'), default='deepseek')
    parser.add_argument('--once', action='store_true', help='运行到第一次暂停后输出 JSON 并退出')
    parser.add_argument('--intent', choices=('explain', 'diagnose', 'verify'), default='diagnose')
    parser.add_argument('--question', default='为什么这个组合逻辑块出现了锁存器？')
    args = parser.parse_args(argv)
    if args.demo:
        print(json.dumps(run_demo(), ensure_ascii=False, indent=2))
        return 0
    try:
        provider = DeepSeekProvider.from_env() if args.provider == 'deepseek' else None
        request = RunRequest('demo', 'r1', 'latch', args.question, args.intent)
    except (ProviderError, ValueError) as error:
        print(f'{error}: 请配置 DEEPSEEK_API_KEY / DEEPSEEK_MODEL；离线可用 --provider rule。', file=sys.stderr)
        return 2
    plan = Plan('demo', 'r1', '复核综合结果', ('eda.synth',)) if args.intent == 'verify' else None
    loop = TeachingLoop(request, FixtureSource(), provider, plan=plan)
    state = loop.run_until_pause()
    if args.once:
        print(json.dumps(asdict(state), ensure_ascii=False, indent=2))
        return 0
    print('开发模式：证据是 fixture；DeepSeek 模式会将问题与该示例摘要发送到 API。')
    commands = {'next': 'hint_next', 'finish': 'finish', 'cancel': 'cancel',
                'reference': 'reference_request', 'refresh': 'refresh_evidence'}
    terminal = {Status.COMPLETED, Status.FAILED, Status.CANCELLED, Status.BUDGET_EXCEEDED}
    shown = 0
    while True:
        for card in state.cards[shown:]:
            print(f'\nL{card.level} [{card.producer}] {card.hint}\n{card.question}')
            print(' / '.join(card.limitations))
        shown = len(state.cards)
        print(f'[{state.status}] stage={state.stage} version={state.version} reason={state.reason or "none"}')
        if state.status in terminal:
            return 0
        if state.status == Status.WAITING_APPROVAL:
            print('验证计划等待可信 UI/Gateway 授权；当前开发入口没有真实执行适配器。')
        try:
            command = input('next / reference / refresh / finish / cancel > ').strip()
        except (EOFError, KeyboardInterrupt):
            loop.act('cancel', uuid4().hex, state.version)
            print('\nCancelled')
            return 0
        if command not in commands:
            print('请输入上述明确动作；聊天文本不能代替执行授权。')
            continue
        try:
            loop.act(commands[command], uuid4().hex, state.version)
            state = loop.run_until_pause()
        except ValueError as error:
            print(f'动作未执行：{error}')


if __name__ == '__main__':
    raise SystemExit(main())
