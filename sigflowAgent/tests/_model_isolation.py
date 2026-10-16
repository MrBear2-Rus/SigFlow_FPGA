"""Keep ordinary fixtures away from the developer's model credentials."""
import os
from pathlib import Path
import tempfile
from unittest.mock import patch


def isolate_model_settings(case):
    directory=tempfile.TemporaryDirectory()
    case.addCleanup(directory.cleanup)
    configuration=patch.dict(os.environ,{'SIGFLOW_DEEPSEEK_CONFIG':str(Path(directory.name)/'missing.json')})
    configuration.start()
    case.addCleanup(configuration.stop)
    for key in ('DEEPSEEK_API_KEY','DEEPSEEK_MODEL','DEEPSEEK_BASE_URL'):
        os.environ.pop(key,None)
