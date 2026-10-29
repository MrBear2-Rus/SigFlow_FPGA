"""Limits for optional learning history; history confers no evidence authority."""
# UTF-8 字节数是离线保守估算单位，不是 DeepSeek tokenizer 的实测 token 数。
# UA-10 默认 1024，普通教学硬上限 1536；本门禁不对事实做字符截断。
MAX_HISTORY_BYTES = 1536


def bounded_history(value):
    if (type(value) is not str or len(value)>MAX_HISTORY_BYTES
            or any(ord(c)<32 and c not in '\n\t' for c in value)):
        return ''
    try:
        return value if len(value.encode('utf-8'))<=MAX_HISTORY_BYTES else ''
    except UnicodeError:
        return ''
