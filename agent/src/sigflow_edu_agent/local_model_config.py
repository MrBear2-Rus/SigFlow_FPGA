"""Optional, explicitly selected local development model configuration."""
import json
import os
from pathlib import Path

_EXPLICIT = ('SIGFLOW_EDU_MODEL_PROVIDER', 'SIGFLOW_EDU_MODEL_API_KEY',
             'SIGFLOW_EDU_MODEL_BASE_URL', 'SIGFLOW_EDU_MODEL_NAME',
             'DEEPSEEK_API_KEY', 'DEEPSEEK_MODEL', 'DEEPSEEK_BASE_URL')


def valid_model_config(data):
    if type(data) is not dict or set(data) != {'api_key', 'model', 'base_url'}:
        return False
    key, model, url = data['api_key'], data['model'], data['base_url']
    return (type(key) is str and 1 <= len(key) <= 512 and
            all(33 <= ord(c) <= 126 for c in key) and
            type(model) is str and 1 <= len(model) <= 128 and
            all(33 <= ord(c) <= 126 for c in model) and
            url in ('https://api.deepseek.com', 'https://api.deepseek.com/v1'))


def load_model_config(data):
    if (os.environ.get('SIGFLOW_EDU_LOCAL_MODEL_DISABLE') or
            any(name in os.environ for name in _EXPLICIT) or not valid_model_config(data)):
        return False
    os.environ.update(SIGFLOW_EDU_MODEL_PROVIDER='deepseek', SIGFLOW_EDU_MODEL_API_KEY=data['api_key'],
                      SIGFLOW_EDU_MODEL_NAME=data['model'], SIGFLOW_EDU_MODEL_BASE_URL=data['base_url'])
    return True


def load_local_model_config():
    """Load a launcher-selected local file only if no model was explicitly set.

    The optional file is data, not executable code. Never print its contents or
    overwrite an explicitly selected model/provider. Missing or invalid files
    leave the original rule-mode behavior intact.
    """
    path = os.environ.get('SIGFLOW_EDU_LOCAL_MODEL_CONFIG', '')
    if not path:
        return False
    try:
        with Path(path).open('rb') as stream:
            raw = stream.read(16385)
        if len(raw) > 16384:
            return False
        data = json.loads(raw)
    except (OSError, ValueError, TypeError):
        return False
    return load_model_config(data)
