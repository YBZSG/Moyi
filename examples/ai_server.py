# 【赖泽豪负责】外部 AI Python 适配服务
# 文件职责：接收 gomoku-ai/v1 棋盘请求，连接不同大模型服务，解析并校验
# AI 返回坐标，处理重试、非法落点纠正和合法候选点约束。
"""gomoku-ai/v1 通用大模型适配器。

Qt 客户端始终请求本服务；本服务把棋盘转换成聊天模型请求，再把模型回答
转换为 {"row": int, "col": int}。

示例：
  http://127.0.0.1:8000/v1/move?provider=search&depth=3
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


SEARCH_WIN_SCORE = 100_000_000
SEARCH_DIRECTIONS = ((0, 1), (1, 0), (1, 1), (1, -1))


class SearchTimeout(RuntimeError):
    """搜索达到本步时间限制。"""


def _inside(size: int, row: int, col: int) -> bool:
    return 0 <= row < size and 0 <= col < size


def _has_five(
    board: list[list[int]],
    size: int,
    row: int,
    col: int,
    player: int,
) -> bool:
    if not _inside(size, row, col) or board[row][col] != player:
        return False
    for delta_row, delta_col in SEARCH_DIRECTIONS:
        count = 1
        for sign in (-1, 1):
            next_row = row + sign * delta_row
            next_col = col + sign * delta_col
            while (
                _inside(size, next_row, next_col)
                and board[next_row][next_col] == player
            ):
                count += 1
                next_row += sign * delta_row
                next_col += sign * delta_col
        if count >= 5:
            return True
    return False


def _would_win(
    board: list[list[int]],
    size: int,
    row: int,
    col: int,
    player: int,
) -> bool:
    if board[row][col] != 0:
        return False
    board[row][col] = player
    won = _has_five(board, size, row, col, player)
    board[row][col] = 0
    return won


def _line_shape_score(count: int, open_ends: int) -> int:
    if count >= 5:
        return SEARCH_WIN_SCORE
    if count == 4:
        return 2_000_000 if open_ends == 2 else 180_000 if open_ends == 1 else 0
    if count == 3:
        return 70_000 if open_ends == 2 else 7_000 if open_ends == 1 else 0
    if count == 2:
        return 4_000 if open_ends == 2 else 450 if open_ends == 1 else 0
    return 100 if open_ends == 2 else 20 if open_ends == 1 else 0


def _placed_move_score(
    board: list[list[int]],
    size: int,
    row: int,
    col: int,
    player: int,
) -> int:
    """评估把 player 放在指定空位后形成的连续棋形。"""
    if board[row][col] != 0:
        return -SEARCH_WIN_SCORE
    board[row][col] = player
    score = 0
    strong_lines = 0
    for delta_row, delta_col in SEARCH_DIRECTIONS:
        count = 1
        open_ends = 0
        for sign in (-1, 1):
            next_row = row + sign * delta_row
            next_col = col + sign * delta_col
            while (
                _inside(size, next_row, next_col)
                and board[next_row][next_col] == player
            ):
                count += 1
                next_row += sign * delta_row
                next_col += sign * delta_col
            if (
                _inside(size, next_row, next_col)
                and board[next_row][next_col] == 0
            ):
                open_ends += 1
        line_score = _line_shape_score(count, open_ends)
        score += line_score
        if (count >= 4 and open_ends >= 1) or (count == 3 and open_ends == 2):
            strong_lines += 1
    board[row][col] = 0
    if strong_lines >= 2:
        score += 500_000
    return score


def _board_score(
    board: list[list[int]],
    size: int,
    perspective: int,
) -> int:
    """按整盘已有连续棋形进行静态估值。"""
    totals = {1: 0, 2: 0}
    for row in range(size):
        for col in range(size):
            player = board[row][col]
            if player == 0:
                continue
            for delta_row, delta_col in SEARCH_DIRECTIONS:
                previous_row = row - delta_row
                previous_col = col - delta_col
                if (
                    _inside(size, previous_row, previous_col)
                    and board[previous_row][previous_col] == player
                ):
                    continue
                count = 0
                next_row, next_col = row, col
                while (
                    _inside(size, next_row, next_col)
                    and board[next_row][next_col] == player
                ):
                    count += 1
                    next_row += delta_row
                    next_col += delta_col
                open_ends = 0
                if (
                    _inside(size, previous_row, previous_col)
                    and board[previous_row][previous_col] == 0
                ):
                    open_ends += 1
                if (
                    _inside(size, next_row, next_col)
                    and board[next_row][next_col] == 0
                ):
                    open_ends += 1
                totals[player] += _line_shape_score(count, open_ends)
    opponent = 3 - perspective
    return totals[perspective] - totals[opponent] * 11 // 10


def _ordered_search_moves(
    board: list[list[int]],
    size: int,
    player: int,
    limit: int,
) -> list[tuple[int, int]]:
    candidates = legal_candidate_moves(board, size, limit=225)
    if not candidates:
        return []

    winning = [
        move
        for move in candidates
        if _would_win(board, size, move[0], move[1], player)
    ]
    if winning:
        return winning

    opponent = 3 - player
    opponent_wins = {
        move
        for move in candidates
        if _would_win(board, size, move[0], move[1], opponent)
    }
    center = size // 2
    scored: list[tuple[int, int, int]] = []
    for row, col in candidates:
        attack = _placed_move_score(board, size, row, col, player)
        defense = _placed_move_score(board, size, row, col, opponent)
        score = attack + defense * 12 // 10
        if (row, col) in opponent_wins:
            score += 20_000_000
        score -= abs(row - center) + abs(col - center)
        scored.append((score, row, col))
    scored.sort(reverse=True)
    return [(row, col) for _, row, col in scored[:limit]]


def choose_search_move(
    payload: dict[str, Any],
    depth: int = 3,
    time_limit: float = 2.5,
) -> tuple[int, int, int]:
    """使用迭代加深 Negamax 与 Alpha-Beta 剪枝计算落点。"""
    board, size, current_player = validate_board(payload)
    depth = max(1, min(int(depth), 5))
    deadline = time.monotonic() + max(0.2, min(float(time_limit), 8.0))
    root_moves = _ordered_search_moves(board, size, current_player, 16)
    if not root_moves:
        raise ValueError("棋盘已满")
    if len(root_moves) == 1:
        return root_moves[0][0], root_moves[0][1], 1

    best_move = root_moves[0]
    completed_depth = 0

    def negamax(
        player: int,
        remaining_depth: int,
        alpha: int,
        beta: int,
        last_move: tuple[int, int] | None,
    ) -> int:
        if time.monotonic() >= deadline:
            raise SearchTimeout
        opponent = 3 - player
        if last_move and _has_five(
            board, size, last_move[0], last_move[1], opponent
        ):
            return -SEARCH_WIN_SCORE - remaining_depth
        if remaining_depth == 0:
            return _board_score(board, size, player)

        width = 10 if remaining_depth >= 3 else 8
        moves = _ordered_search_moves(board, size, player, width)
        if not moves:
            return 0
        best = -SEARCH_WIN_SCORE * 2
        for row, col in moves:
            board[row][col] = player
            try:
                value = -negamax(
                    opponent,
                    remaining_depth - 1,
                    -beta,
                    -alpha,
                    (row, col),
                )
            finally:
                board[row][col] = 0
            best = max(best, value)
            alpha = max(alpha, value)
            if alpha >= beta:
                break
        return best

    for target_depth in range(1, depth + 1):
        try:
            iteration_best = best_move
            iteration_score = -SEARCH_WIN_SCORE * 2
            alpha = -SEARCH_WIN_SCORE * 2
            for row, col in root_moves:
                if time.monotonic() >= deadline:
                    raise SearchTimeout
                board[row][col] = current_player
                try:
                    score = -negamax(
                        3 - current_player,
                        target_depth - 1,
                        -SEARCH_WIN_SCORE * 2,
                        -alpha,
                        (row, col),
                    )
                finally:
                    board[row][col] = 0
                if score > iteration_score:
                    iteration_score = score
                    iteration_best = (row, col)
                alpha = max(alpha, score)
            best_move = iteration_best
            completed_depth = target_depth
            root_moves.remove(best_move)
            root_moves.insert(0, best_move)
        except SearchTimeout:
            break

    return best_move[0], best_move[1], max(completed_depth, 1)


def ranked_model_candidate_moves(
    board: list[list[int]],
    size: int,
    current_player: int,
    limit: int = 48,
) -> list[tuple[int, int]]:
    """为聊天模型生成兼顾进攻与防守的候选点。"""
    candidates = legal_candidate_moves(board, size, limit=225)
    if not candidates:
        return []

    winning = [
        move
        for move in candidates
        if _would_win(board, size, move[0], move[1], current_player)
    ]
    if winning:
        return winning

    opponent = 3 - current_player
    forced_blocks = [
        move
        for move in candidates
        if _would_win(board, size, move[0], move[1], opponent)
    ]
    if forced_blocks:
        return forced_blocks

    center = size // 2
    scored: list[tuple[int, int, int]] = []
    for row, col in candidates:
        attack = _placed_move_score(
            board, size, row, col, current_player
        )
        defense = _placed_move_score(board, size, row, col, opponent)
        combined = attack + defense * 14 // 10
        combined -= abs(row - center) + abs(col - center)
        scored.append((combined, row, col))
    scored.sort(reverse=True)
    return [(row, col) for _, row, col in scored[:limit]]


def build_messages(
    payload: dict[str, Any],
    candidate_moves: list[tuple[int, int]] | None = None,
) -> list[dict[str, str]]:
    board, size, current_player = validate_board(payload)
    candidates = candidate_moves or ranked_model_candidate_moves(
        board, size, current_player
    )
    opponent = 3 - current_player
    own_wins = {
        move
        for move in candidates
        if _would_win(board, size, move[0], move[1], current_player)
    }
    opponent_wins = {
        move
        for move in candidates
        if _would_win(board, size, move[0], move[1], opponent)
    }
    candidate_lines = []
    for index, (row, col) in enumerate(candidates):
        attack = _placed_move_score(
            board, size, row, col, current_player
        )
        defense = _placed_move_score(board, size, row, col, opponent)
        if (row, col) in own_wins:
            label = "立即取胜，必须优先"
        elif (row, col) in opponent_wins:
            label = "阻止对手下一手获胜，必须防守"
        elif defense >= 70_000:
            label = "高防守威胁"
        elif attack >= 70_000:
            label = "高进攻威胁"
        else:
            label = "普通候选"
        candidate_lines.append(
            f"M{index}: row={row}, col={col}, "
            f"进攻分={attack}, 防守分={defense}, 标签={label}"
        )
    candidate_text = "\n".join(candidate_lines)
    board_text = "\n".join(" ".join(str(cell) for cell in row) for row in board)
    color = "黑棋" if current_player == 1 else "白棋"
    history = payload.get("history", [])
    last_move_text = "无（当前为开局）"
    if isinstance(history, list) and history:
        last_move = history[-1]
        if isinstance(last_move, dict):
            last_move_text = (
                f"对手刚刚下在 row={last_move.get('row')}, "
                f"col={last_move.get('col')}。必须重新评估该落点产生的威胁。"
            )
    system_prompt = (
        "你是五子棋落子引擎。棋盘值 0=空位、1=黑棋、2=白棋；坐标从 0 开始，"
        "row 表示从上到下，col 表示从左到右。优先立即取胜，其次阻止对手立即"
        "取胜；如果候选标签包含“必须防守”，必须阻止对手下一步获胜，不允许"
        "继续自己的普通进攻；之后再考虑活四、冲四、活三、双重威胁和中心控制。"
        "防守分表示该位置对对手的潜在价值，不得忽略。候选着法已经过合法性"
        "检查，你必须只选择一个候选编号。只输出 JSON，禁止解释，禁止自行输出"
        "或修改 row、col。"
    )
    user_prompt = (
        f"棋盘大小：{size}x{size}\n"
        f"当前执子：{color}（值 {current_player}）\n"
        f"对手棋子值：{opponent}\n"
        f"对手最后一步：{last_move_text}\n"
        f"目标：五子连珠，长连也算胜利。\n"
        f"棋盘：\n{board_text}\n"
        "合法候选着法：\n"
        f"{candidate_text}\n"
        '请严格返回：{"moveId":"M编号"}'
    )
    return [
        {"role": "system", "content": system_prompt},
        {"role": "user", "content": user_prompt},
    ]


def extract_candidate_move(
    content: Any,
    candidates: list[tuple[int, int]],
    board: list[list[int]],
    size: int,
) -> tuple[int, int]:
    """优先解析候选编号，同时兼容旧模型返回的 row/col。"""
    data: Any = content
    text = ""
    if not isinstance(data, dict):
        text = str(content).strip()
        if text.startswith("```"):
            text = re.sub(r"^```(?:json)?\s*", "", text, flags=re.IGNORECASE)
            text = re.sub(r"\s*```$", "", text)
        text = re.sub(r",\s*([}\]])", r"\1", text)
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            data = None

    direct_move_id: Any = None
    if data is None:
        direct_match = re.fullmatch(
            r"[\"']?M?(\d+)[\"']?",
            text,
            flags=re.IGNORECASE,
        )
        if direct_match:
            direct_move_id = direct_match.group(1)

    if isinstance(data, dict):
        if isinstance(data.get("move"), dict):
            data = data["move"]
        move_id = data.get(
            "moveId",
            data.get(
                "move_id",
                data.get(
                    "candidateId",
                    data.get("candidate", data.get("move")),
                ),
            ),
        )
    else:
        move_id = direct_move_id

    if move_id is not None:
        match = re.fullmatch(r"M?(\d+)", str(move_id).strip(),
                             flags=re.IGNORECASE)
        if not match:
            raise InvalidModelMove("模型返回的 moveId 格式无效")
        index = int(match.group(1))
        if not 0 <= index < len(candidates):
            raise InvalidModelMove(
                f"模型返回的 moveId M{index} 不在候选列表中"
            )
        row, col = candidates[index]
        if board[row][col] != 0:
            raise InvalidModelMove("候选落点在等待期间已被占用")
        return row, col

    # 兼容仍然返回 {"row":...,"col":...} 的模型与自定义服务。
    return extract_move(content, board, size)


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
            "max_tokens": 80,
            "stream": False,
            "response_format": {"type": "json_object"},
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
            unsupported_json_mode = (
                error.code == 400
                and "response_format" in request_data
                and any(
                    keyword in error_message.lower()
                    for keyword in (
                        "response_format",
                        "json mode",
                        "json_object",
                        "unsupported",
                    )
                )
            )
            if unsupported_json_mode:
                # 部分自定义 OpenAI 兼容服务不支持 JSON Mode；
                # 去掉该可选参数后仍使用严格提示词重试。
                request_data.pop("response_format", None)
                continue
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
    board, size, current_player = validate_board(payload)
    candidate_moves = ranked_model_candidate_moves(
        board,
        size,
        current_player,
    )
    candidate_ids = ", ".join(
        f"M{index}" for index in range(len(candidate_moves))
    )
    messages = build_messages(payload, candidate_moves)
    last_error: InvalidModelMove | None = None

    # 大模型偶尔会选中已有棋子。第一次非法时把具体原因反馈给模型，
    # 再请求一次，而不是立即让整局切换到内置 AI。
    for attempt in range(2):
        content = request_chat_content(messages, provider, api_key, model)
        try:
            row, col = extract_candidate_move(
                content,
                candidate_moves,
                board,
                size,
            )
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
                            "只选择候选列表中的另一个编号，并严格返回："
                            '{"moveId":"M编号"}'
                        ),
                    },
                ]
            )
            messages[-1]["content"] += (
                "\n允许的编号只有："
                f"{candidate_ids}"
            )

    # 上游已经真实调用，但连续给出非法落点。适配器在本地选择一个合法点，
    # 避免 Qt 把本局误判为接口永久不可用；下一手仍会继续调用外部模型。
    if not candidate_moves:
        raise InvalidModelMove("棋盘上没有合法候选点")
    fallback_row, fallback_col = candidate_moves[0]
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
    if provider_name == "search":
        depth_text = query.get("depth", ["3"])[0].strip()
        try:
            depth = int(depth_text)
        except ValueError as error:
            raise ValueError("search 模式的 depth 必须是整数") from error
        if not 1 <= depth <= 5:
            raise ValueError("search 模式的 depth 必须在 1 至 5 之间")
        return "search", None, str(depth)
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
            supported = ", ".join(
                ["demo", "search", *PROVIDERS.keys(), "custom"]
            )
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
    server_version = "GomokuAIAdapter/2.2"

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
                "modelMoveFormat": "moveId",
                "modelTactics": "attack-defense-constrained",
                "providers": ["demo", "search", *PROVIDERS.keys(), "custom"],
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

            if provider_name == "demo":
                row, col = choose_demo_move(payload)
                detail = "Python 协议演示"
            elif provider_name == "search":
                depth = int(model)
                row, col, reached_depth = choose_search_move(
                    payload,
                    depth=depth,
                )
                detail = (
                    f"Python Alpha-Beta 搜索 AI"
                    f"（目标深度 {depth}，完成 {reached_depth}）"
                )
            else:
                assert provider is not None
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
