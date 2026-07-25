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
| Python 强搜索 AI | `http://127.0.0.1:8000/v1/move?provider=search&depth=3` | 留空 |
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

`search` 模式使用独立的 Python 棋类搜索算法，不调用云端接口。它采用迭代
加深、Negamax、Alpha-Beta 剪枝、候选着法排序和威胁棋形估值。`depth` 可设为
`1`～`5`，默认推荐 `3`；深度越大，通常棋力越强，但思考时间也越长。

不带 `provider` 的地址属于协议演示模式，仅用于验证接口连通性。

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

### 云端聊天模型的合法落点约束

Python 适配器不会再要求 DeepSeek、通义千问等聊天模型直接填写 `row` 和
`col`。适配器先生成已经校验过的合法候选点，为其分配 `M0、M1……` 编号，
模型只需返回：

```json
{"moveId": "M12"}
```

适配器随后把编号映射回真实坐标。这可以避免模型把行列写反、返回越界坐标或
选择已占用位置。对于仍返回 `row/col` 的旧模型和自定义服务，适配器继续保留
兼容解析；无效编号会携带原因自动重试一次。

为避免聊天模型只顾自己进攻，候选点还会附带进攻分、防守分和战术标签，提示
中会单独标出对手最后一步。如果当前一方能够立即取胜，适配器只提供取胜点；
如果对手下一步能够获胜，适配器只提供必须封堵的点。普通局面仍由模型根据
带评分的候选列表作出选择。

如果需要让云端模型完全独立决策，可在地址中加入 `mode=raw`：

```text
http://127.0.0.1:8000/v1/move?provider=deepseek&mode=raw
```

此模式仍会提交完整 15×15 棋盘、当前执子方、对手最后落点和历史数据，但不会
计算或提供攻防评分，不会排序候选点，也不会强制模型封堵。适配器仅为棋盘上的
全部空位生成 `moveId`，用于保证最终坐标合法。模型连续两次返回无效编号时，
适配器会如实报告失败，而不会用 Python 策略替模型落子。

适配器可以从 `<think>…</think>`、说明文字、Markdown JSON 代码块以及
`moveId=M12` 等常见推理模型输出中提取最终编号。若战术约束模式最终需要本地
纠正，Qt 状态栏会显示具体原因，例如编号越界、格式无效或落点已占用。
