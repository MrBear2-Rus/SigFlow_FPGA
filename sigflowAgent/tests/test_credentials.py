from contextlib import redirect_stdout, redirect_stderr
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from sigflow_edu_agent.deepseek import DeepSeekConfig, DeepSeekProvider
from sigflow_edu_agent.domain import ProviderError


class CredentialTests(unittest.TestCase):
    def setUp(self):
        self.directory=tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path=Path(self.directory.name)/'private'/'credentials.json'

    def test_missing_file_does_not_make_a_network_request(self):
        from sigflow_edu_agent.credentials import load_config
        self.assertIsNone(load_config(self.path))

    @unittest.skipUnless(os.name=='nt','DPAPI is a Windows current-user store')
    def test_real_dpapi_roundtrip_stores_no_plaintext_key(self):
        from sigflow_edu_agent.credentials import save_config,load_config
        config=DeepSeekConfig('fake-secret-for-local-roundtrip','test-model')
        save_config(config,self.path)
        self.assertEqual(load_config(self.path),config)
        self.assertNotIn(config.api_key.encode(),self.path.read_bytes())
        self.assertNotIn(config.model.encode(),self.path.read_bytes())
        self.assertNotIn(config.api_key,repr(load_config(self.path)))
        save_config(DeepSeekConfig('replacement-test-key','other-model'),self.path)
        self.assertEqual(load_config(self.path).model,'other-model')
        self.assertEqual(len(list(self.path.parent.iterdir())),1)

    def test_malformed_or_oversized_file_has_only_fixed_errors(self):
        from sigflow_edu_agent.credentials import load_config,CredentialError
        self.path.parent.mkdir()
        for data in (b'private-text-not-json',b'{}',b'{"schema":"wrong","encrypted":"aaa"}',b'x'*16385):
            self.path.write_bytes(data)
            with self.assertRaises(CredentialError) as result:
                load_config(self.path)
            self.assertEqual(str(result.exception),'key_store_invalid')
            self.assertNotIn(str(self.path),str(result.exception))

    @unittest.skipUnless(os.name=='nt','DPAPI is a Windows current-user store')
    def test_tampered_ciphertext_is_not_silently_accepted(self):
        from sigflow_edu_agent.credentials import save_config,load_config,CredentialError
        import base64
        save_config(DeepSeekConfig('test-key','test-model'),self.path)
        body=json.loads(self.path.read_text())
        blob=bytearray(base64.b64decode(body['encrypted']))
        blob[-1]^=1
        body['encrypted']=base64.b64encode(blob).decode()
        self.path.write_text(json.dumps(body))
        with self.assertRaises(CredentialError):
            load_config(self.path)

    @unittest.skipUnless(os.name=='nt','DPAPI is a Windows current-user store')
    def test_failed_atomic_replace_keeps_previous_configuration(self):
        from sigflow_edu_agent.credentials import save_config,load_config,CredentialError
        original=DeepSeekConfig('original-fake-key','original-model')
        save_config(original,self.path)
        with patch('sigflow_edu_agent.credentials.os.replace',side_effect=OSError('private-file-detail')):
            with self.assertRaisesRegex(CredentialError,'key_store_write_failed'):
                save_config(DeepSeekConfig('new-fake-key','new-model'),self.path)
        self.assertEqual(load_config(self.path),original)
        self.assertEqual(len(list(self.path.parent.iterdir())),1)

    def test_environment_wins_without_mixing_saved_values(self):
        with patch.dict(os.environ,{'DEEPSEEK_API_KEY':'env-key','DEEPSEEK_MODEL':'env-model'},clear=True), \
                patch('sigflow_edu_agent.credentials.load_config') as load:
            self.assertEqual(DeepSeekProvider.from_settings().config.model,'env-model')
            load.assert_not_called()
        with patch.dict(os.environ,{'DEEPSEEK_MODEL':'partial'},clear=True), \
                patch('sigflow_edu_agent.credentials.load_config') as load:
            with self.assertRaises(ProviderError):
                DeepSeekProvider.from_settings()
            load.assert_not_called()

    def test_saved_config_is_used_without_environment_credentials(self):
        config=DeepSeekConfig('saved-key','saved-model')
        with patch.dict(os.environ,{},clear=True),patch('sigflow_edu_agent.credentials.load_config',return_value=config):
            self.assertEqual(DeepSeekProvider.from_settings().config,config)

    def test_non_windows_has_no_plaintext_persistence_fallback(self):
        from sigflow_edu_agent.credentials import save_config,CredentialError
        with patch('sigflow_edu_agent.credentials.os.name','posix'):
            with self.assertRaisesRegex(CredentialError,'key_store_unsupported'):
                save_config(DeepSeekConfig('test-key','m'),self.path)
        self.assertFalse(self.path.exists())

    def test_saved_provider_supplies_host_secret_redaction_material_without_network(self):
        from sigflow_edu_agent.host.factory import make_factory
        from sigflow_edu_agent.clients.gateway import GatewayConfig
        provider=DeepSeekProvider(DeepSeekConfig('saved-redaction-key','m'))
        with patch.object(DeepSeekProvider,'from_settings',return_value=provider):
            factory=make_factory(GatewayConfig(12345,'gateway-fixture-token','inst-1'))
        self.assertTrue(factory.model_available)
        self.assertEqual(factory.model_secrets,('saved-redaction-key',))

    def test_fixture_isolation_ignores_saved_and_environment_user_credentials(self):
        from _model_isolation import isolate_model_settings
        case=unittest.TestCase()
        with patch.dict(os.environ,{'DEEPSEEK_API_KEY':'not-for-fixtures','DEEPSEEK_MODEL':'user-model',
                                   'SIGFLOW_DEEPSEEK_CONFIG':'user-controlled-path'}):
            try:
                isolate_model_settings(case)
                self.assertNotIn('DEEPSEEK_API_KEY',os.environ)
                self.assertNotIn('DEEPSEEK_MODEL',os.environ)
                self.assertFalse(Path(os.environ['SIGFLOW_DEEPSEEK_CONFIG']).exists())
                with self.assertRaises(ProviderError):
                    DeepSeekProvider.from_settings()
            finally:
                case.doCleanups()


class ConfigCLITests(unittest.TestCase):
    def invoke(self,args):
        from sigflow_edu_agent.model_config import main
        out,error=io.StringIO(),io.StringIO()
        with redirect_stdout(out),redirect_stderr(error):
            result=main(args)
        return result,out.getvalue(),error.getvalue()

    def test_configure_uses_hidden_prompt_and_never_echoes_key(self):
        with patch('sigflow_edu_agent.model_config.getpass.getpass',return_value='private-prompt-key'), \
                patch('sigflow_edu_agent.model_config.save_config') as save:
            code,out,error=self.invoke(['configure','--model','test-model'])
        self.assertEqual((code,error),(0,''))
        self.assertEqual(save.call_args.args[0].api_key,'private-prompt-key')
        self.assertNotIn('private-prompt-key',out)

    def test_status_does_not_contact_provider_or_print_key(self):
        provider=DeepSeekProvider(DeepSeekConfig('private-status-key','m'))
        with patch('sigflow_edu_agent.model_config.DeepSeekProvider.from_settings',return_value=provider), \
                patch.object(DeepSeekProvider,'generate') as generate:
            code,out,error=self.invoke(['status'])
        self.assertEqual((code,error),(0,''))
        self.assertTrue(json.loads(out)['configured'])
        self.assertNotIn('private-status-key',out)
        generate.assert_not_called()

    def test_real_test_path_uses_one_provider_call_and_checks_card(self):
        provider=DeepSeekProvider(DeepSeekConfig('private-smoke-key','m'))
        def generate(context):
            return dict(level=1,hint='报告观察到存储行为。',question='输出需要记忆吗？',evidence_ids=[context.evidence.evidence_id])
        with patch('sigflow_edu_agent.model_config.DeepSeekProvider.from_settings',return_value=provider), \
                patch.object(provider,'generate',side_effect=generate) as generate_mock:
            code,out,error=self.invoke(['test'])
        self.assertEqual((code,error),(0,''))
        self.assertEqual(json.loads(out)['producer'],'model')
        self.assertEqual(json.loads(out)['evidence_origin'],'fixture')
        generate_mock.assert_called_once()
        self.assertNotIn('private-smoke-key',out)

    def test_api_failure_is_explicit_and_never_falls_back_or_leaks(self):
        provider=DeepSeekProvider(DeepSeekConfig('private-smoke-key','m'))
        with patch('sigflow_edu_agent.model_config.DeepSeekProvider.from_settings',return_value=provider), \
                patch.object(provider,'generate',side_effect=ProviderError('model_auth_failed')) as generate:
            code,out,error=self.invoke(['test'])
        self.assertEqual((code,out,error.strip()),(3,'','model_auth_failed'))
        generate.assert_called_once()

    def test_invalid_card_does_not_report_success_or_fallback(self):
        provider=DeepSeekProvider(DeepSeekConfig('fake-key','m'))
        with patch('sigflow_edu_agent.model_config.DeepSeekProvider.from_settings',return_value=provider), \
                patch.object(provider,'generate',return_value={'unsafe':'candidate'}):
            code,out,error=self.invoke(['test'])
        self.assertEqual((code,out,error.strip()),(3,'','invalid_output'))

    def test_key_echo_in_candidate_is_redacted_before_json_output(self):
        provider=DeepSeekProvider(DeepSeekConfig('private-echo-key','m'))
        def generate(context):
            return dict(level=1,hint='private-echo-key',question='请核对观察。',evidence_ids=[context.evidence.evidence_id])
        with patch('sigflow_edu_agent.model_config.DeepSeekProvider.from_settings',return_value=provider), \
                patch.object(provider,'generate',side_effect=generate):
            code,out,error=self.invoke(['test'])
        self.assertEqual((code,error),(0,''))
        self.assertNotIn('private-echo-key',out)
        self.assertIn('[redacted]',out)

    def test_insecure_prompt_fallback_is_refused(self):
        import getpass,warnings
        def prompt(*args):
            warnings.warn('unsafe terminal',getpass.GetPassWarning)
        with patch('sigflow_edu_agent.model_config.getpass.getpass',side_effect=prompt), \
                patch('sigflow_edu_agent.model_config.save_config') as save:
            code,out,error=self.invoke(['configure','--model','m'])
        self.assertEqual((code,out,error.strip()),(2,'','hidden_input_unavailable'))
        save.assert_not_called()


if __name__=='__main__':
    unittest.main()
