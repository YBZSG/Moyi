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
F:\Qt\Tools\CMake_64\bin\cmake.exe -S . -B build\release `
  -G "MinGW Makefiles" `
  -DCMAKE_PREFIX_PATH=F:\Qt\6.11.1\mingw_64 `
  -DCMAKE_BUILD_TYPE=Release
F:\Qt\Tools\CMake_64\bin\cmake.exe --build build\release -j
```

也可以直接用 Qt Creator 打开 `CMakeLists.txt` 后运行。

## 外部 AI

在人机模式中选择“外部 HTTP AI”，填写服务地址。程序会按
`gomoku-ai/v1` 协议发送完整棋盘；接口不可用时不会卡死，而会自动改用内置 AI。
随项目提供的 Python 适配器支持 DeepSeek、通义千问、Moonshot、智谱 GLM、
Ollama 和自定义 OpenAI 兼容接口。

完整协议和 Python 示例见 [docs/AI_API.md](docs/AI_API.md)。

代码内也提供了 `AiProvider` 抽象类。若 AI 模型直接以 C++ 库形式集成，只需派生该类，
实现 `requestMove()`，成功时发出 `moveReady()`，失败时发出 `failed()`。

## 测试

```powershell
F:\Qt\Tools\CMake_64\bin\ctest.exe --test-dir build\release --output-on-failure
```

测试覆盖棋盘状态、悔棋、横线/对角线胜负判定、AI 取胜与封堵，以及
`gomoku-ai/v1` 的真实本地 HTTP 请求/响应。
