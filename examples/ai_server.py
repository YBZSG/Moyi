# 【组员一负责】外部 AI Python 适配服务
# 文件职责：接收 gomoku-ai/v1 棋盘请求，连接不同大模型服务，解析并校验
# AI 返回坐标，处理重试、非法落点纠正和合法候选点约束。
"""gomoku-ai/v1 通用大模型适配器。

Qt 客户端始终请求本服务；本服务把棋盘转换成聊天模型请求，再把模型回答
转换为 {"row": int, "col": int}。

示例：
  http://127.0.0.1:8000/v1/move?provider=deepseek
  http://127.0.0.1:8000/v1/move?provider=qwen
  http://127.0.0.1:8000/v1/move?provider=moonshot
  http://127.0.0.1:8000/v1/move?provider=ollama&model=qwen3

API Key 可以由 Qt 的 Bearer Token 输入框传入，也可以放在环境变量
GOMOKU_AI_API_KEY 中。服务只监听 127.0.0.1。
"""

from __future__ import annotations

import json
import os
import re
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.parse import parse_qs, urlparse


HOST = "127.0.0.1"
PORT = 8000
UPSTREAM_TIMEOUT_SECONDS = 25


@dataclass(frozen=True)
class Provider:
    api_url: str
    default_model: str
    protocol: str = "openai-compatible"
    requires_key: bool = True


PROVIDERS: dict[str, Provider] = {
    "deepseek": Provider(
        "https://api.deepseek.com/chat/completions",
        "deepseek-chat",
    ),
    "qwen": Provider(
        "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions",
        "qwen-plus",
    ),
    "moonshot": Provider(
        "https://api.moonshot.cn/v1/chat/completions",
        "moonshot-v1-8k",
    ),
    "zhipu": Provider(
        "https://open.bigmodel.cn/api/paas/v4/chat/completions",
        "glm-4-flash",
    ),
    "ollama": Provider(
        "http://127.0.0.1:11434/api/chat",
        "qwen3",
        protocol="ollama",
        requires_key=False,
    ),
}


class UpstreamError(RuntimeError):
    """上游模型服务调用失败。"""


class InvalidModelMove(ValueError):
    """模型没有返回合法空位。"""


def validate_board(payload: dict[str, Any]) -> tuple[list[list[int]], int, int]:
    board = payload.get("board")
    size = int(payload.get("boardSize", 15))
    current_player = int(payload.get("currentPlayer", 2))

    if size < 5 or size > 25:
        raise ValueError("boardSize 超出支持范围")
    if not isinstance(board, list) or len(board) != size:
        raise ValueError(f"board 必须包含 {size} 行")
    if current_player not in (1, 2):
        raise ValueError("currentPlayer 必须是 1 或 2")

    normalized: list[list[int]] = []
    for row in board:
        if not isinstance(row, list) or len(row) != size:
            raise ValueError(f"board 每行必须包含 {size} 列")
        normalized_row = [int(cell) for cell in row]
        if any(cell not in (0, 1, 2) for cell in normalized_row):
            raise ValueError("棋盘单元只能是 0、1、2")
        normalized.append(normalized_row)
    return normalized, size, current_player


def choose_demo_move(payload: dict[str, Any]) -> tuple[int, int]:
    """无外部模型时的协议演示策略：选择最靠近中心的空位。"""
    board, size, _ = validate_board(payload)
    center = size // 2
    empty = [
        (row, col)
        for row in range(size)
        for col in range(size)
        if board[row][col] == 0
    ]
    if not empty:
        raise ValueError("棋盘已满")
    return min(
        empty,
        key=lambda point: (
            abs(point[0] - center) + abs(point[1] - center),
            point[0],
            point[1],
        ),
    )


def legal_candidate_moves(
    board: list[list[int]],
    size: int,
    limit: int = 96,
) -> list[tuple[int, int]]:
    """Return stable nearby legal moves that a language model can copy."""
    occupied = [
        (row, col)
        for row in range(size)
        for col in range(size)
        if board[row][col] != 0
    ]
    center = size // 2
    if not occupied:
        return [(center, center)]

    candidates: set[tuple[int, int]] = set()
    for stone_row, stone_col in occupied:
        for row in range(max(0, stone_row - 2), min(size, stone_row + 3)):
            for col in range(max(0, stone_col - 2), min(size, stone_col + 3)):
                if board[row][col] == 0:
                    candidates.add((row, col))

    return sorted(
        candidates,
        key=lambda point: (
            min(
                max(abs(point[0] - row), abs(point[1] - col))
                for row, col in occupied
            ),
            abs(point[0] - center) + abs(point[1] - center),
            point[0],
            point[1],
        ),
    )[:limit]


def build_messages(payload: dict[str, Any]) -> list[dict[str, str]]:
    board, size, current_player = validate_board(payload)
    candidate_text = json.dumps(
        legal_candidate_moves(board, size),
        ensure_ascii=False,
    )
    board_text = "\n".join(" ".join(str(cell) for cell in row) for row in board)
    color = "黑棋" if current_player == 1 else "白棋"
    system_prompt = (
        "你是五子棋落子引擎。棋盘值 0=空位、1=黑棋、2=白棋；坐标从 0 开始，"
        "row 表示从上到下，col 表示从左到右。请选择一个空位，优先立即取胜，"
        "其次阻止对手立即取胜，再考虑攻防棋形。只输出 JSON，禁止解释。"
    )
    user_prompt = (
        f"棋盘大小：{size}x{size}\n"
        f"当前执子：{color}（值 {current_player}）\n"
        f"目标：五子连珠，长连也算胜利。\n"
        f"棋盘：\n{board_text}\n"
        '请严格返回：{"row":整数,"col":整数}'
    )
    system_prompt += (
        " You MUST copy exactly one coordinate from the legal candidate list. "
        "Never invent a coordinate outside that list."
    )
    user_prompt += (
        "\nLegal candidate coordinates (choose exactly one):\n"
        f"{candidate_text}"
    )
    return [
        {"role": "system", "content": system_prompt},
        {"role": "user", "content": user_prompt},
    ]


def extract_move(content: Any, board: list[list[int]], size: int) -> tuple[int, int]:
    if isinstance(content, dict):
        data = content
    else:
        text = str(content).strip()
        if text.startswith("```"):
            text = re.sub(r"^```(?:json)?\s*", "", text, flags=re.IGNORECASE)
            text = re.sub(r"\s*```$", "", text)
        text = re.sub(r",\s*([}\]])", r"\1", text)
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            match = re.search(r"\{.*?\}", text, flags=re.DOTALL)
            if match:
                try:
                    object_text = re.sub(
                        r",\s*([}\]])",
                        r"\1",
                        match.group(0),
                    )
                    data = json.loads(object_text)
                except json.JSONDecodeError as error:
                    raise InvalidModelMove("模型返回的落点 JSON 无效") from error
            else:
                coordinate_match = re.search(
                    r"row\s*[:=]\s*[\"']?(-?\d+)[\"']?.*?"
                    r"col\s*[:=]\s*[\"']?(-?\d+)",
                    text,
                    flags=re.IGNORECASE | re.DOTALL,
                )
                if not coordinate_match:
                    raise InvalidModelMove("模型没有返回 JSON 落点")
                data = {
                    "row": int(coordinate_match.group(1)),
                    "col": int(coordinate_match.group(2)),
                }

    if not isinstance(data, dict):
        raise InvalidModelMove("模型返回的落点必须是 JSON 对象")
    if isinstance(data.get("move"), dict):
        data = data["move"]

    def coordinate(value: Any) -> int | None:
        if isinstance(value, bool):
            return None
        if isinstance(value, int):
            return value
        if isinstance(value, float) and value.is_integer():
            return int(value)
        if isinstance(value, str) and re.fullmatch(r"-?\d+", value.strip()):
            return int(value.strip())
        return None

    row = coordinate(data.get("row"))
    col = coordinate(data.get("col"))
    if row is None or col is None:
        raise InvalidModelMove("模型返回的 row/col 必须是整数")
    if not (0 <= row < size and 0 <= col < size):
        raise InvalidModelMove(f"模型返回的落点 ({row}, {col}) 超出棋盘")
    if board[row][col] != 0:
        raise InvalidModelMove(f"模型返回的落点 ({row}, {col}) 已有棋子")
    return row, col


def request_chat_content(
    messages: list[dict[str, str]],
    provider: Provider,
    api_key: str,
    model: str,
) -> Any:
    if provider.protocol == "ollama":
        request_data = {
            "model": model,
            "messages": messages,
            "stream": False,
            "format": "json",
        }
    else:
        request_data = {
            "model": model,
            "messages": messages,
            "temperature": 0.1,
            "max_tokens": 300,
            "stream": False,
        }

    headers = {
        "Content-Type": "application/json",
        "Accept": "application/json",
        "User-Agent": "Qt-Gomoku-AI-Adapter/1.0",
    }
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"

    response_data: dict[str, Any] | None = None
    transient_statuses = {408, 425, 429, 500, 502, 503, 504}
    for request_attempt in range(3):
        request = urllib.request.Request(
            provider.api_url,
            data=json.dumps(request_data, ensure_ascii=False).encode("utf-8"),
            headers=headers,
            method="POST",
        )
        try:
            with urllib.request.urlopen(
                request,
                timeout=UPSTREAM_TIMEOUT_SECONDS,
            ) as response:
                response_data = json.loads(response.read().decode("utf-8"))
            break
        except urllib.error.HTTPError as error:
            error_body = error.read().decode("utf-8", errors="replace")
            error_message = ""
            try:
                error_json = json.loads(error_body)
                error_value = error_json.get("error", "")
                if isinstance(error_value, dict):
                    error_message = str(error_value.get("message", ""))
                else:
                    error_message = str(error_value)
            except json.JSONDecodeError:
                error_message = error_body
            error_message = re.sub(r"\s+", " ", error_message).strip()[:180]
            if error.code in transient_statuses and request_attempt < 2:
                time.sleep(0.8 * (request_attempt + 1))
                continue
            detail = f"：{error_message}" if error_message else ""
            raise UpstreamError(
                f"上游 AI 返回 HTTP {error.code}{detail}"
            ) from error
        except urllib.error.URLError as error:
            if request_attempt < 1:
                time.sleep(0.6)
                continue
            raise UpstreamError(f"无法连接上游 AI：{error.reason}") from error
        except (TimeoutError, json.JSONDecodeError) as error:
            if request_attempt < 1:
                time.sleep(0.6)
                continue
            raise UpstreamError("上游 AI 超时或返回了无效 JSON") from error

    if response_data is None:
        raise UpstreamError("上游 AI 没有返回数据")

    try:
        if provider.protocol == "ollama":
            return response_data["message"]["content"]
        return response_data["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError) as error:
        raise UpstreamError("上游 AI 响应中缺少 message.content") from error


def call_upstream(
    payload: dict[str, Any],
    provider: Provider,
    api_key: str,
    model: str,
) -> tuple[int, int, bool]:
    board, size, _ = validate_board(payload)
    candidate_text = json.dumps(
        legal_candidate_moves(board, size),
        ensure_ascii=False,
    )
    messages = build_messages(payload)
    last_error: InvalidModelMove | None = None

    # 大模型偶尔会选中已有棋子。第一次非法时把具体原因反馈给模型，
    # 再请求一次，而不是立即让整局切换到内置 AI。
    for attempt in range(2):
        content = request_chat_content(messages, provider, api_key, model)
        try:
            row, col = extract_move(content, board, size)
            return row, col, False
        except InvalidModelMove as error:
            last_error = error
            if attempt == 1:
                break
            messages.extend(
                [
                    {"role": "assistant", "content": str(content)},
                    {
                        "role": "user",
                        "content": (
                            f"刚才的落点非法：{error}。请重新检查棋盘，"
                            '只返回另一个合法空位：{"row":整数,"col":整数}'
                        ),
                    },
                ]
            )
            messages[-1]["content"] += (
                "\nYou MUST copy exactly one coordinate from this legal list: "
                f"{candidate_text}"
            )

    # 上游已经真实调用，但连续给出非法落点。适配器在本地选择一个合法点，
    # 避免 Qt 把本局误判为接口永久不可用；下一手仍会继续调用外部模型。
    fallback_row, fallback_col = choose_demo_move(payload)
    return fallback_row, fallback_col, True


def resolve_provider(
    query: dict[str, list[str]],
) -> tuple[str, Provider | None, str]:
    provider_name = query.get(
        "provider",
        [os.getenv("GOMOKU_AI_PROVIDER", "demo")],
    )[0].strip().lower()

    if provider_name in ("", "demo", "example"):
        return "demo", None, "demo"
    if provider_name == "custom":
        api_url = os.getenv("GOMOKU_AI_URL", "").strip()
        default_model = os.getenv("GOMOKU_AI_MODEL", "").strip()
        protocol = os.getenv("GOMOKU_AI_PROTOCOL", "openai-compatible").strip()
        if not api_url or not default_model:
            raise ValueError(
                "custom 模式需要设置 GOMOKU_AI_URL 和 GOMOKU_AI_MODEL"
            )
        provider = Provider(
            api_url,
            default_model,
            protocol=protocol,
            requires_key=protocol != "ollama",
        )
    else:
        provider = PROVIDERS.get(provider_name)
        if provider is None:
            supported = ", ".join(["demo", *PROVIDERS.keys(), "custom"])
            raise ValueError(f"未知 provider；支持：{supported}")

    model = query.get(
        "model",
        [os.getenv("GOMOKU_AI_MODEL", provider.default_model)],
    )[0].strip()
    if not model:
        raise ValueError("模型名称不能为空")
    return provider_name, provider, model


def bearer_token(headers: Any) -> str:
    authorization = headers.get("Authorization", "").strip()
    if authorization.lower().startswith("bearer "):
        return authorization[7:].strip()
    return os.getenv("GOMOKU_AI_API_KEY", "").strip()


class GomokuHandler(BaseHTTPRequestHandler):
    server_version = "GomokuAIAdapter/2.0"

    def do_GET(self) -> None:  # noqa: N802 - HTTP handler API
        parsed = urlparse(self.path)
        if parsed.path != "/health":
            self.send_error(404, "Not Found")
            return
        self._send_json(
            200,
            {
                "status": "ok",
                "protocol": "gomoku-ai/v1",
                "providers": ["demo", *PROVIDERS.keys(), "custom"],
            },
        )

    def do_POST(self) -> None:  # noqa: N802 - HTTP handler API
        parsed = urlparse(self.path)
        if parsed.path != "/v1/move":
            self.send_error(404, "Not Found")
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
            payload = json.loads(self.rfile.read(length))
            provider_name, provider, model = resolve_provider(parse_qs(parsed.query))

            if provider is None:
                row, col = choose_demo_move(payload)
                detail = "Python 协议演示"
            else:
                api_key = bearer_token(self.headers)
                if provider.requires_key and not api_key:
                    self._send_json(
                        401,
                        {"error": f"{provider_name} 模式缺少 API Key"},
                    )
                    return
                row, col, used_adapter_fallback = call_upstream(
                    payload,
                    provider,
                    api_key,
                    model,
                )
                detail = f"{provider_name}/{model}"
                if used_adapter_fallback:
                    detail += "（模型落点非法，适配器已纠正）"

            self._send_json(
                200,
                {"row": row, "col": col, "message": detail},
            )
        except InvalidModelMove as error:
            self._send_json(502, {"error": str(error)})
        except UpstreamError as error:
            self._send_json(502, {"error": str(error)})
        except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
            self._send_json(400, {"error": str(error)})

    def log_message(self, fmt: str, *args: object) -> None:
        print(f"[gomoku-ai] {self.address_string()} - {fmt % args}")

    def _send_json(self, status: int, data: dict[str, Any]) -> None:
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


if __name__ == "__main__":
    server = ThreadingHTTPServer((HOST, PORT), GomokuHandler)
    print(f"Gomoku AI adapter: http://{HOST}:{PORT}/v1/move")
    print("Health check:       http://127.0.0.1:8000/health")
    print("Press Ctrl+C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n服务已停止")
    finally:
        server.server_close()
