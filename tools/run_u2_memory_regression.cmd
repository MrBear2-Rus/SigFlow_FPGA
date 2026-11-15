@echo off
setlocal
set "u2root=%~dp0.."
set "u2python=%u2root%\sigflowAgent\.venv\Scripts\python.exe"
if defined SIGFLOW_EDU_AGENT_PYTHON set "u2python=%SIGFLOW_EDU_AGENT_PYTHON%"
if not exist "%u2python%" (
    echo Set SIGFLOW_EDU_AGENT_PYTHON to an absolute Python executable path.
    exit /b 2
)
set "PYTHONPATH=%u2root%\sigflowAgent;%u2root%\sigflowAgent\ua10_module\src;%u2root%\sigflowAgent\ua10_module\vendor;%u2root%\agent\src"
"%u2python%" -P -m unittest discover -s "%u2root%\sigflowAgent\tests" -p test_ua10_reintegration.py -v
exit /b %ERRORLEVEL%
