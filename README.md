# 墨弈

一个基于 Qt Widgets 的完整 15×15 五子棋项目，支持本地双人对战、内置策略 AI
和可替换的外部 HTTP AI。

## 功能

- 黑棋先行，点击交叉点落子，自动切换回合
- 横、竖、主对角线、副对角线五子连珠判胜
- 双人对战、人机对战（玩家执黑）和双机自动对战切换
- 双机对战支持暂停/继续；悔棋时自动暂停，避免外部模型持续产生调用
- 双机对战可为黑方、白方分别配置独立的内置/外部 AI、接口地址和 Token
- 内置策略 AI：优先取胜、封堵对手，并根据活二/活三/活四评分
- 重新开始（`R`）和悔棋（`Z`）；人机模式按完整回合悔棋
- 最后落子标记、获胜连线、和棋判定
- 外部 AI 超时、断网、非法落点时自动回退内置 AI
- 外部大模型返回非法落点时会携带错误原因自动重试一次；失败仅回退当前一步

## 构建

项目依赖 Qt 6（同时兼容 Qt 5）的 `Widgets` 和 `Network` 模块。

```powershell
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

如果 CMake 无法自动找到 Qt，请把 Qt 安装目录传给 `CMAKE_PREFIX_PATH`，例如：

```powershell
cmake -S . -B build/release `
  -DCMAKE_PREFIX_PATH="C:\Qt\6.8.0\mingw_64" `
  -DCMAKE_BUILD_TYPE=Release
```

请按自己的 Qt 版本和实际安装位置修改路径。也可以直接用 Qt Creator 打开
`CMakeLists.txt`，选择工具包后构建运行。

## 外部 AI

`gomoku-ai/v1` 是本项目定义的棋盘通信协议，不是需要另外下载的 AI 模型。
程序将完整棋盘发送给外部 HTTP 服务，再读取该服务返回的落子坐标。接口超时、
断网、返回格式错误或落点非法时，当前一步会自动改用内置 AI。

### 使用项目自带的 Python 适配器

此方法可以连接 DeepSeek、通义千问、Moonshot、智谱 GLM、本地 Ollama，以及
其他兼容 OpenAI Chat Completions 格式的服务。适配器只使用 Python 标准库，
不需要安装额外依赖。

1. 安装 Python 3.9 或更高版本。启动“墨弈”时，程序会自动检查
   `127.0.0.1:8000`，端口未被占用时会自动启动 EXE 同目录下的
   `ai_server.py`，通常不需要手动操作。

   如果自动启动失败，可在项目根目录手动运行：

   ```powershell
   python examples/ai_server.py
   ```

   看到 `Gomoku AI adapter: http://127.0.0.1:8000/v1/move` 即表示启动成功。
   手动启动时，运行游戏期间不要关闭这个终端窗口。

2. 打开“墨弈”，选择“人机对战”或“双机对战”，将对应棋手的 AI 类型设为
   “外部 HTTP AI”。

3. 根据使用的模型填写接口地址和 Token：

   | 模型服务 | 接口地址 | Token |
   |---|---|---|
   | 本地协议演示 | `http://127.0.0.1:8000/v1/move` | 留空 |
   | DeepSeek | `http://127.0.0.1:8000/v1/move?provider=deepseek` | DeepSeek API Key |
   | 通义千问 | `http://127.0.0.1:8000/v1/move?provider=qwen` | DashScope API Key |
   | Moonshot | `http://127.0.0.1:8000/v1/move?provider=moonshot` | Moonshot API Key |
   | 智谱 GLM | `http://127.0.0.1:8000/v1/move?provider=zhipu` | 智谱 API Key |
   | 本地 Ollama | `http://127.0.0.1:8000/v1/move?provider=ollama&model=qwen3` | 留空 |

   Token 只通过请求传给本机适配器，不会写入项目文件。双机对战可以分别为
   黑棋和白棋设置不同的地址、模型和 Token。

4. 开始对局。如果想先检查适配器是否运行正常，可在浏览器打开
   `http://127.0.0.1:8000/health`。

使用 Ollama 时，需要先启动 Ollama 并下载相应模型，例如：

```powershell
ollama pull qwen3
ollama serve
```

如需指定云端模型，可在接口地址末尾增加 `&model=模型名`，例如：

```text
http://127.0.0.1:8000/v1/move?provider=deepseek&model=deepseek-reasoner
```

### 连接自己开发的 AI

也可以不使用 Python 适配器，直接让自己的服务接收 `POST` 请求。将服务地址
填入游戏，只要它能按照 `gomoku-ai/v1` 接收棋盘，并返回如下 JSON 即可：

```json
{"row": 7, "col": 8, "message": "模型说明（可选）"}
```

坐标从 `0` 开始，范围为 `0`～`14`，并且必须指向空位。

完整协议和 Python 示例见 [docs/AI_API.md](docs/AI_API.md)。

代码内也提供了 `AiProvider` 抽象类。若 AI 模型直接以 C++ 库形式集成，只需派生该类，
实现 `requestMove()`，成功时发出 `moveReady()`，失败时发出 `failed()`。

## 测试

```powershell
ctest --test-dir build/release --output-on-failure
```

测试覆盖棋盘状态、悔棋、横线/对角线胜负判定、AI 取胜与封堵，以及
外部 AI 接口的请求格式、响应解析和本地模拟 HTTP 通信。该自动测试不会消耗
任何云端大模型额度。
