import json
import unittest
from unittest.mock import patch

from sigflow_edu_agent.deepseek import DeepSeekConfig, DeepSeekProvider
from sigflow_edu_agent.domain import CardContext, Evidence, ProviderError, RunRequest
from sigflow_edu_agent.loop import TeachingLoop


def context():
    return CardContext('为什么有锁存器？', 1,
                       Evidence('ev1', 'p', 'r1', 'job1', 'latch', True, '示例报告', True))


def response(content=None, **updates):
    if content is None:
        content = json.dumps({'level': 1, 'hint': '报告观察到存储行为。',
                              'question': '输出需要记忆吗？', 'evidence_ids': ['ev1']})
    choice = {'finish_reason': 'stop', 'message': {'role': 'assistant', 'content': content}}
    choice.update(updates)
    return json.dumps({'choices': [choice], 'usage': {'total_tokens': 40}}).encode()


class DeepSeekTests(unittest.TestCase):
    def setUp(self):
        self.network = patch('sigflow_edu_agent.deepseek.http.client.HTTPSConnection')
        self.factory = self.network.start()
        self.addCleanup(self.network.stop)
        self.connection = self.factory.return_value
        self.reply = self.connection.getresponse.return_value
        self.reply.status = 200
        self.reply.read.return_value = response()
        self.config = DeepSeekConfig('test-secret-never-log', 'test-model')

    def test_request_targets_deepseek_with_json_and_no_stream_or_tools(self):
        result = DeepSeekProvider(self.config).generate(context())
        args, kwargs = self.connection.request.call_args
        self.assertEqual(args[:2], ('POST', '/chat/completions'))
        self.assertEqual(kwargs['headers']['Authorization'], 'Bearer test-secret-never-log')
        body = json.loads(kwargs['body'])
        self.assertEqual(body['model'], 'test-model')
        self.assertFalse(body['stream'])
        self.assertEqual(body['response_format'], {'type': 'json_object'})
        self.assertNotIn('tools', body)
        self.assertIn('JSON', body['messages'][0]['content'])
        self.assertNotIn('test-secret-never-log', kwargs['body'].decode())
        self.assertEqual(result['level'], 1)
        self.factory.assert_called_once_with('api.deepseek.com', timeout=30.0)
        self.connection.close.assert_called_once()

    def test_api_configuration_is_read_from_environment_without_repr_secret(self):
        with patch.dict('os.environ', {'DEEPSEEK_API_KEY': 'secret-env', 'DEEPSEEK_MODEL': 'chosen-model', 'DEEPSEEK_BASE_URL': 'https://api.deepseek.com/v1'}, clear=True):
            provider = DeepSeekProvider.from_env()
        provider.generate(context())
        self.assertEqual(self.connection.request.call_args.args[:2], ('POST', '/v1/chat/completions'))
        self.assertNotIn('secret-env', repr(provider.config))

    def test_absent_key_or_model_is_explicit_not_a_network_attempt(self):
        for env in ({}, {'DEEPSEEK_MODEL': 'm'}, {'DEEPSEEK_API_KEY': 'secret'}):
            with patch.dict('os.environ', env, clear=True), self.assertRaisesRegex(ProviderError, 'model_unconfigured'):
                DeepSeekProvider.from_env()
        self.factory.assert_not_called()

    def test_invalid_endpoint_or_timeout_rejected_without_exposing_values(self):
        for options in ({'base_url': 'https://attacker.invalid/?secret=abc'}, {'base_url': 'http://api.deepseek.com'}, {'timeout': 0}, {'timeout': float('nan')}, {'max_tokens': 0}):
            with self.subTest(options=options), self.assertRaises(ProviderError) as error:
                DeepSeekConfig('secret', 'm', **options)
            self.assertNotIn('secret', str(error.exception))
        self.factory.assert_not_called()

    def test_http_error_is_classified_without_body_and_without_retry(self):
        for status, code in ((401, 'model_auth_failed'), (429, 'model_rate_limited'), (500, 'model_unavailable'), (302, 'model_unavailable')):
            self.reply.status = status
            self.reply.read.reset_mock()
            with self.assertRaisesRegex(ProviderError, code):
                DeepSeekProvider(self.config).generate(context())
            self.reply.read.assert_not_called()
        self.assertEqual(self.connection.request.call_count, 4)

    def test_timeout_and_transport_error_do_not_expose_request(self):
        for error, code in ((TimeoutError('secret'), 'model_timeout'), (OSError('secret'), 'model_unavailable')):
            self.connection.request.side_effect = error
            with self.assertRaisesRegex(ProviderError, code) as caught:
                DeepSeekProvider(self.config).generate(context())
            self.assertNotIn('secret', str(caught.exception))

    def test_empty_truncated_or_tool_response_is_rejected(self):
        for raw in (b'not json', b'{}', response(''), response('[]'), response('null'),
                    response(finish_reason='length'),
                    response(message={'content': '{}', 'tool_calls': [{'id': 'bad'}]})):
            self.reply.read.return_value = raw
            with self.subTest(raw=raw), self.assertRaisesRegex(ProviderError, 'invalid_output'):
                DeepSeekProvider(self.config).generate(context())

    def test_large_output_and_context_are_bounded(self):
        self.reply.read.return_value = b'x' * (262144 + 1)
        with self.assertRaisesRegex(ProviderError, 'response_too_large'):
            DeepSeekProvider(self.config).generate(context())
        ctx = context()
        large = CardContext('x' * 20000, 1, ctx.evidence)
        self.factory.reset_mock()
        with self.assertRaisesRegex(ProviderError, 'context_too_large'):
            DeepSeekProvider(self.config).generate(large)
        self.factory.assert_not_called()

    def test_real_provider_boundary_falls_back_inside_loop_on_api_failure(self):
        self.reply.status = 503
        class Source:
            def collect(self, project_id, revision):
                return context().evidence
        loop = TeachingLoop(RunRequest('p', 'r1', 'i', '问题'), Source(), DeepSeekProvider(self.config))
        state = loop.run_until_pause()
        self.assertEqual(state.cards[-1].producer, 'rule')
        self.assertEqual(state.reason, 'model_unavailable')
        self.assertEqual(self.connection.request.call_count, 1)


if __name__ == '__main__':
    unittest.main()
