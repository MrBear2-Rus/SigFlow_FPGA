# UA-09 Gemma Dense 安装声明

Gemma Dense 是可选的本地课程检索模式。独立 U2 Host 通过 `--course-mode gemma_dense` 启用。桌面 Agent 的课程检索默认使用 FTS5，常规安装无需模型和推理依赖。

本仓库提交检索代码、依赖约束、配置示例和测试。模型权重及其下载目录、生成的向量索引、本地配置、测试缓存和密钥由使用者保存在本地。现有课程库 `data/course.sqlite` 继续作为输入，索引构建产物使用单独路径。

## 安装和运行

在 `sigflowAgent` 目录运行以下 PowerShell 命令。验证环境为 Windows、Python 3.11、CPU 和 `constraints-gemma-tested.txt` 固定的推理依赖。

```powershell
py -3.11 -m venv .venv
$py = '.\.venv\Scripts\python.exe'
& $py -m pip install -c constraints-gemma-tested.txt './embedding_module[inference]'
& $py -m pip install -c constraints-gemma-tested.txt '.[gemma]'
```

模型由 Google DeepMind 发布，采用 Apache 2.0 许可证。下面的命令固定了与已测试权重 SHA-256 一致的官方修订版。下载的模型权重约 1.49 GB。运行时读取本地模型，使用文本编码器并关闭视觉和音频编码器。[官方模型与许可](https://huggingface.co/google/embeddinggemma-2/tree/f9972ac22e696a105c482202646979e33e4b5a1e)

```powershell
& .\.venv\Scripts\hf.exe download google/embeddinggemma-2 --revision f9972ac22e696a105c482202646979e33e4b5a1e --local-dir ./local_models/embeddinggemma-2
Copy-Item settings.example.json settings.local.json
& $py tools/build_gemma_index.py --course-db ./data/course.sqlite --model ./local_models/embeddinggemma-2 --output ./data/course-gemma-v2.sqlite
& $py tools/unified_demo.py ./settings.local.json '为什么组合逻辑会推断出锁存器？' --mode gemma_dense
```

Demo 只执行本地检索，不需要 DeepSeek 密钥。若模型目录或课程库与示例不同，请同时更新构建参数与 `settings.local.json`。索引记录模型资产指纹和课程版本。更换模型目录内容或课程库版本后，构建到新路径并更新配置。构建命令拒绝覆盖现有索引。

独立 U2 Host 可在已有启动参数后追加以下选项。

```text
--course-mode gemma_dense --gemma-model ./local_models/embeddinggemma-2 --gemma-index ./data/course-gemma-v2.sqlite
```

## 无模型的最小测试

新环境安装共享基础包和教学核心即可。测试使用合成课程数据库，覆盖词法检索、融合、引用与索引合同校验。

```powershell
& $py -m pip install ./embedding_module .
& $py -m unittest discover -s tests -p test_public_unified.py
```

Windows 本地推理和课程适配器已通过真实查询。完整桌面链路和从零下载官方模型再建索引的流程尚未在全新环境复测。
