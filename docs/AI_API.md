# 五子棋外部 AI 接口（gomoku-ai/v1）

## 请求

客户端向界面中填写的地址发送 `POST` 请求：

```http
Content-Type: application/json
Accept: application/json
X-Gomoku-Protocol: gomoku-ai/v1
Authorization: Bearer <可选 Token>
```

请求体示例：

```json
{
  "protocol": "gomoku-ai/v1",
  "requestId": "41f3d8b9-28d3-4e90-82ac-d12bb4c765e4",
  "boardSize": 15,
  "board": [
    [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
  ],
  "currentPlayer": 2,
  "currentPlayerName": "white",
  "history": [
    {"row": 7, "col": 7, "player": 1}
  ],
  "rules": {
    "winLength": 5,
    "overlineWins": true
  }
}
```

`board` 实际固定为 15 行、每行 15 个整数。棋子编码如下：

| 值 | 含义 |
|---:|---|
| 0 | 空位 |
| 1 | 黑棋 |
| 2 | 白棋 |

坐标均从 `0` 开始；左上角为 `(row=0, col=0)`，右下角为 `(14, 14)`。

## 响应

服务返回 HTTP 2xx 和 JSON：

```json
{
  "row": 7,
  "col": 8,
  "message": "Neural MCTS / depth 800"
}
```

也支持嵌套写法：

```json
{
  "move": {"row": 7, "col": 8},
  "message": "model-v2"
}
```

`row`、`col` 必须指向棋盘内的空位。客户端默认等待 60 秒；超时、HTTP 错误、
JSON 错误或非法落点都会自动回退到内置 AI。

## 最小 Python 服务

项目中的 `examples/ai_server.py` 只使用 Python 标准库，既可验证协议，也可转发
到常见大模型：

```powershell
python examples\ai_server.py
```

启动后，根据模型在程序中填写地址：

| 模型服务 | Qt 中填写的地址 | Token |
|---|---|---|
| 协议演示 | `http://127.0.0.1:8000/v1/move` | 留空 |
| DeepSeek | `http://127.0.0.1:8000/v1/move?provider=deepseek` | DeepSeek API Key |
| 通义千问 | `http://127.0.0.1:8000/v1/move?provider=qwen` | DashScope API Key |
| Moonshot | `http://127.0.0.1:8000/v1/move?provider=moonshot` | Moonshot API Key |
| 智谱 GLM | `http://127.0.0.1:8000/v1/move?provider=zhipu` | 智谱 API Key |
| 本地 Ollama | `http://127.0.0.1:8000/v1/move?provider=ollama&model=qwen3` | 留空 |

也可以在地址后用 `&model=模型名` 覆盖默认模型。例如：

```text
http://127.0.0.1:8000/v1/move?provider=deepseek&model=deepseek-reasoner
```

API Key 通过 Qt 的密码输入框传给本机适配器，不会写入项目文件。

### 自定义 OpenAI 兼容接口

启动服务前设置环境变量：

```powershell
$env:GOMOKU_AI_URL = "https://example.com/v1/chat/completions"
$env:GOMOKU_AI_MODEL = "your-model"
python examples\ai_server.py
```

Qt 中填写：

```text
http://127.0.0.1:8000/v1/move?provider=custom
```

API Key 仍填写在 Qt 的 Token 框。适配器还提供健康检查：

```text
http://127.0.0.1:8000/health
```

如果要接入非聊天接口的神经网络或 MCTS，也可以增加新的 provider：

1. 从 `payload["board"]` 生成模型输入；
2. 用模型计算合法落点；
3. 返回 `(row, col)`。

模型运行在 Python、C++、远程 GPU 服务或云端均可，只要遵守此 JSON 协议。
