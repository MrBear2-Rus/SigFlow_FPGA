"""复用宿主已经管理的结构化模型服务，不创建 HTTP 客户端或读取密钥。"""
from typing import Any, Protocol

from ..errors import ConfigurationError, PatternGenerationError


class JSONCompletionService(Protocol):
    """宿主负责模型、超时、重试、限流、费用账本和网络授权。"""

    def complete_json(self, messages: list[dict[str, str]], *, max_tokens: int) -> dict[str, Any]: ...


class HostPatternProvider:
    """将宿主通用 JSON 服务适配为既有 PatternProvider。"""

    def __init__(self, client: JSONCompletionService, *, model_id: str = 'unknown-host-model',
                 is_network: bool = True, synthetic_only: bool = True, max_tokens: int = 1024):
        if not callable(getattr(client, 'complete_json', None)):
            raise ConfigurationError('宿主必须提供 complete_json，教学卡片 generate(context) 不能直接替代')
        if getattr(client, 'requires_host_reservation', False):
            raise ConfigurationError('须先绑定宿主授权和预算服务，不能直接注入底层模型客户端')
        if any(type(x) is not bool for x in (is_network, synthetic_only)):
            raise ConfigurationError('联网与数据访问开关必须显式使用布尔值')
        if type(max_tokens) is not int or not 1 <= max_tokens <= 2048:
            raise ConfigurationError('输出预算必须为 1..2048 的整数')
        if not isinstance(model_id, str) or not 1 <= len(model_id) <= 128:
            raise ConfigurationError('模型来源标识无效')
        self._client = client
        self.model_id = model_id  # 只记录来源，不选择或覆盖宿主模型。
        self.is_network = is_network
        self.synthetic_only = synthetic_only
        self.max_tokens = max_tokens

    def generate(self, messages: list[dict[str, str]], *, max_tokens: int) -> dict[str, Any]:
        if type(max_tokens) is not int or not 1 <= max_tokens <= self.max_tokens:
            raise ConfigurationError('调用超过已配置的输出上限')
        # 不捕获或重试宿主错误；宿主既有异常与费用统计原样生效。
        value = self._client.complete_json(messages, max_tokens=max_tokens)
        if not isinstance(value, dict) or not value:
            raise PatternGenerationError('宿主结构化模型服务未返回非空 JSON 对象')
        return value
